# Design

The plugin is split by GStreamer base class while sharing one ACL runtime and
one memory abstraction. VPC and JPEG operations are frame transforms. VDEC and
VENC are asynchronous codec elements whose callback completes a frame-owned
pending context; VDEC defers frame completion during EOS so the GStreamer
streaming thread owns the final `finish_frame` calls.

```text
GStreamer buffer
      |
      v
[caps + stride validation]
      |
      +--> [system memory] --copy--> [AscendDVPP memory]
      |                                  |
      |                                  v
      |                          [ACL DVPP channel]
      |                                  |
      |              +-------------------+-------------------+
      |              |                                       |
      v              v                                       v
 [VPC filter]   [JPEGD/JPEGE]                         [VDEC/VENC]
      |              |                                       |
      +--------------+-------------------+-------------------+
                                         |
                           [in-flight job queue]
                         submit N -> retire oldest
                                         |
                                         v
                              [GStreamer output buffer]
```

The mock backend replaces device allocation and ACL calls with host allocation
and copies. It is intentionally limited to ABI and negotiation validation.

## Runtime ownership

The shared runtime counts users per device and tracks every device activated
during the current ACL lifetime. When this plugin owns `aclInit`, final release
resets all activated devices before calling `aclFinalize`; it does not finalize
an ACL runtime initialized by another component. A generation counter invalidates
thread-local device selections after the runtime lifetime ends.

## Codec report worker

VDEC and VENC deliver completion callbacks through a CANN report-processing
thread. Each codec element owns one worker and binds its operating-system thread
ID before creating the DVPP channel:

```text
[codec set_format]
       |
       v
[start report worker] --publish TID--> [set channel-desc thread ID]
       |                                             |
       |                                             v
       |                                      [create channel]
       |                                             |
       +<------------ enable processing --------------+
       |
       v
[aclrtProcessReport loop] ---> [VDEC/VENC callback] ---> [pending frame]
       |
       +--> request stop -> destroy channel -> join worker
```

VDEC binds its report worker thread ID to the channel descriptor and sends an
explicit EOS descriptor before waiting for pending callbacks. VENC uses the
channel-bound report worker plus a dedicated ACL stream subscribed with
`aclrtSubscribeReport`; that stream is unsubscribed and destroyed during
release. The VDEC channel must be destroyed while its report worker is still
alive, then the worker is joined, so channel teardown can finish its pending
report work.

## VPC optimization record

The original VPC path allocated temporary input/output DVPP memory for every
frame, copied system memory to the device, submitted one VPC request, waited
for the stream, and copied the result back. The implementation now has two
explicit paths:

```text
system input -> reusable DVPP input pool -> async H2D -> VPC stream[i]
DVPP input  ---------------------------------------> VPC stream[i]
                                                    |
DVPP output pool <----------------------------------+
system output <- sync oldest job <- D2H -----------+
```

`ascendvideoconvert` exposes `inflight-depth` (default 4). Jobs retain their
input buffer and ACL descriptors until retirement, and EOS drains pending jobs
before the stream and device memory are released. The output pool reserves
`2 * inflight-depth + 4` buffers so a downstream converter can retain upstream
outputs while both queues are in flight.

### Wide input crop and resize

`ascendvideoconvert` exposes `crop-left`, `crop-right`, `crop-top`, and
`crop-bottom` as pixel counts removed from the input edges. The output width
and height are negotiated through the output caps. CANN ROI coordinates are
inclusive, so the converter derives `right = input_width - crop_right - 1`
and `bottom = input_height - crop_bottom - 1`; left/top must be even and
right/bottom must be odd.

The validated wide-input geometry is:

```text
main:    BGR 4240x514, stride 12720x514, ROI (0,4239,0,513)
         -> NV12 4096x496, stride 4096x496

preview: BGR 4240x514, stride 12720x514, ROI (20,4219,50,463)
         -> cropped source 4200x414
         -> NV12 4096x404, stride 4096x404
```

On Ascend 310P, a packed input wider than 4096 enters the product's 8K VPC
path, where BGR888 is not a supported input format. The converter therefore
uses this explicit device-only path for the 4240-wide input:

```text
[BGR DVPP input 12720-byte rows]
                 |
                 +-- device-to-device row copies --> [BGR tile 0 <= 4096]
                 |                                      |
                 |                                      +-- VPC crop/resize/paste --+
                 +----------------------------------> [BGR tile 1 <= 4096]          |
                                                        |                            v
                                                        +-- VPC crop/resize/paste -> [NV12 output]
```

Each tile has its own DVPP allocation and picture descriptor. Its crop ROI
covers the tile, and its paste ROI writes a disjoint horizontal range of the
shared output descriptor. The pending job owns both tile allocations, all ROI
objects, the input/output buffers, and the ACL stream until synchronization;
cleanup synchronizes a failed submission before releasing tile memory. No
`videocrop`, `videoscale`, host-side image transform, or silent CPU fallback is
used.

The element also exposes `stream-count` (default 2, maximum 4). Jobs select a
stream in round-robin order and synchronize their own stream on retirement.
Runtime activation caches the selected device per thread and tags the cache
with a runtime generation, so a later reset/reinitialize cannot reuse stale
device state.

Validated on the current `IT21PD2G10` target with CANN 9.1.0-beta.1:

- Mock allocator and device-pointer tests: passed.
- Ascend VPC depth 1 and depth 4: EOS passed.
- Two chained converters: first output was `output-device=1`; second input was
  `input-device=1`, confirming device-memory reuse without an intermediate
  Host copy.
- With upstream allocation proposal enabled, `videotestsrc` produced buffers
  recognized as `input-device=1` by the converter. An 8-branch 1920x1080@60
  stress run reached stable DVPP VPC 5%; the same topology at 30 fps reached
  2%. The source still performs its own mapped write, while hardware-aware
  upstream elements can fill the DVPP pool without a Host input buffer.
- With `stream-count=1` and `stream-count=2`, the same 8-branch 60 fps run
  remained at stable DVPP VPC 5%. This workload is source-rate and small-output
  limited; more ACL streams did not raise device utilization.
- A six-branch H.264 A/B run used the same decoded NV12 input for CPU
  `videoscale`/`videoconvert` and DVPP VPC conversion, with both paths producing
  640x480 BGR. Host GStreamer CPU fell from 417% to 100% (about 76% lower);
  DVPP VPC averaged 1.5% and device CtrlCPU averaged 4%. Decode and common
  NV12 preparation stayed on the host.
- The previous 16-branch synchronous stress run reached 17-18% VPC. Async
  stress runs are not yet a saturation benchmark because large system-memory
  source pipelines spend significant time in source/pool startup and preroll.

Codec smoke validation on the current `IT21PD2G10` target also passed:

- Four 320x240 NV12 frames encoded through `ascendh264enc` and reached EOS.
- Four 320x240 H.264 Annex-B access units decoded through `ascendh264dec`,
  including EOS drain and channel teardown.
- Four 320x240 NV12 frames encoded through `ascendh265enc`; the output was
  independently identified as H.265/HEVC before decode.
- Four H.265 Annex-B access units decoded through `ascendh265dec`, including
  EOS drain and channel teardown. Broader profile/rate-control combinations
  remain outside the validated matrix.
