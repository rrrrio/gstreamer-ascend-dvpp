# Offload Wide-Input Crop, Resize, CSC, and Encode

This ExecPlan is a living document. It follows the repository's
`execplan/references/PLANS.md` methodology. The sections `Progress`,
`Surprises & Discoveries`, `Decision Log`, and `Outcomes & Retrospective` must
remain current as implementation and hardware validation proceed.

## Purpose / Big Picture

The publishing pipeline supplies a CPU-generated `4240x514` BGR frame. After
this change, `ascendvideoconvert` can crop and resize that frame on DVPP while
also converting BGR to NV12, so one branch can produce `4096x496` NV12 for
H.265 and another can crop 20 pixels from each side and 50 pixels from the top
and bottom before producing `4096x404` NV12 for H.264. The CPU `videocrop` and
`videoscale` elements are not part of these branches.

The result is visible with finite GStreamer pipelines that reach EOS, produce
the requested raw geometries before the hardware encoders, and run both tee
branches concurrently.

## Progress

- [x] (2026-10-13 00:00 +0800) Confirmed the worktree is clean and found no
  interrupted build, GStreamer, or NPU process.
- [x] (2026-10-13 00:00 +0800) Confirmed privileged read-only device access:
  `/dev/davinci0`, `/dev/davinci1`, `/dev/devmm_svm`, product `IT21PD2G10`,
  NPU ID `64`, and two chips.
- [x] (2026-10-13 00:00 +0800) Read the installed CANN 9.1.0-beta.1 DVPP
  declarations. The relevant VPC APIs support crop-plus-resize, BGR888,
  NV12, a maximum width stride of 16384 bytes, and a maximum height stride of
  4096 lines.
- [x] (2026-10-13 01:00 +0800) Added explicit geometry validation and crop
  properties to
  `src/gstascendvideoconvert.c`.
- [x] (2026-10-13 01:00 +0800) Replaced the resize-only ACL request with a
  crop-and-resize ACL request
  while preserving asynchronous job ownership and device memory lifetimes.
- [x] (2026-10-13 01:00 +0800) Added mock, parser, integration, and
  hardware-sized tests for the two
  full-frame and cropped branches and the concurrent tee topology.
- [x] (2026-10-13 01:00 +0800) Ran mock and CANN Meson gates, privileged
  hardware EOS tests, and the
  real-plugin `gst_parse_launch` publishing-pipeline parser test.
- [ ] Update English and Chinese documentation with exact geometry, stride,
  ROI, device, command, and result evidence.
- [ ] Run repository hooks and create the required commit.

## Surprises & Discoveries

- Observation: The previous runtime probe reported no device only because it
  ran without the granted device permissions.
  Evidence: A privileged read-only probe found all required device nodes and
  `npu-smi info -l` reported `IT21PD2G10`, NPU ID `64`, chip count `2`.
- Observation: A valid byte stride is not sufficient for a 310P VPC request.
  Evidence: the official VPC restrictions treat input wider than 4096 as an
  8K path, whose supported input format list does not include BGR888. The
  direct `4240x514 BGR` request failed on this target with `507018`
  (`E39999: Inner Error`, AICPU exception).
- Observation: The wide packed BGR input works when it is split in device
  memory before VPC submission.
  Evidence: the implementation copies each source row with device-to-device
  ACL copies into two BGR tiles, then submits two
  `acldvppVpcCropResizePasteAsync` operations into one NV12 output. Main VPC,
  preview VPC, both encoders, and the concurrent tee reached EOS on `IT21PD2G10`.
- Observation: DVPP ROI coordinates use inclusive boundaries rather than
  four independent crop lengths.
  Evidence: CANN requires left/top to be even and right/bottom to be odd.
  The preview ROI is `(left=20, right=4219, top=50, bottom=463)`.

## Decision Log

- Decision: Expose `crop-left`, `crop-right`, `crop-top`, and `crop-bottom` as
  clear GObject properties. Continue to express target width and height with
  negotiated output caps.
  Rationale: The two tee branches need independent static ROIs, and explicit
  properties are inspectable in `gst-launch-1.0` and do not require upstream
  metadata producers. The output caps already carry the target geometry.
  Date/Author: 2026-10-13 / Yuhao Ding.
- Decision: Use `acldvppVpcCropResizeAsync` for inputs within the non-8K VPC
  path, and use device-side tiling with
  `acldvppVpcCropResizePasteAsync` for packed input wider than 4096.
  Rationale: 310P does not accept BGR888 in its >4096 input path. Tiling keeps
  crop, resize, and BGR-to-NV12 CSC on DVPP and avoids a CPU fallback.
  Date/Author: 2026-10-13 / Yuhao Ding.
- Decision: Validate DVPP picture strides from pixel format, not just visible
  width. BGR/RGB use `align16(width * 3)`, four-channel formats use
  `align16(width * 4)`, and semiplanar YUV uses `align16(width)`.
  Rationale: The existing implementation incorrectly used `align16(width)`
  for BGR, which would describe a 4240-wide image with an invalid stride and
  insufficient allocation.
  Date/Author: 2026-10-13 / Yuhao Ding.
- Decision: Reject geometry that cannot be represented by the DVPP stride or
  ROI rules; do not silently fall back to CPU.
  Rationale: Packed input above 4096 is handled by a bounded two-tile path,
  while unsupported height, stride, parity, or tile geometry must fail with a
  GStreamer error.
  Date/Author: 2026-10-13 / Yuhao Ding.

## Outcomes & Retrospective

The mock build and all five mock tests pass. The CANN build and all five CANN
tests pass on product `IT21PD2G10` (NPU ID 64, plugin device 0). The physical
main VPC, preview VPC, main H.265, preview H.264, and concurrent H.264/H.265
tee tests each reached EOS. The parser test accepts the registered publishing
pipeline. Broader VENC profiles, rate-control combinations, and output pixel
quality are outside this finite EOS matrix.

## Context and Orientation

`src/gstascendvideoconvert.c` is a `GstVideoFilter` that receives raw frames,
allocates or reuses DVPP memory, submits asynchronous VPC work, and retires
the oldest job after stream synchronization. `src/gstascendruntime.c` creates
ACL picture descriptors and owns the `acldvppMalloc`/`acldvppFree` boundary.
`src/gstascendvideocodec.c` accepts NV12 and sends it to the H.264 or H.265
VENC channel. `tests/test_ascend_video_convert.c` is the existing finite
GStreamer test harness.

The installed CANN 9.1.0-beta.1 header at
`/usr/local/Ascend/cann-9.1.0-beta.1/aarch64-linux/include/acl/ops/acl_dvpp.h`
states the following constraints used by this plan:

    BGR888 input: width_stride = align16(width * 3)
    NV12 output:  width_stride = align16(width)
    height_stride: input height aligned to 2, maximum 4096
    width_stride: minimum 32, maximum 16384
    ROI: left/top even, right/bottom odd, inclusive coordinates

For the requested paths:

    main:    BGR 4240x514, stride 12720x514, full ROI 0,4239,0,513
             -> NV12 4096x496, stride 4096x496
    preview: BGR 4240x514, stride 12720x514, ROI 20,4219,50,463
             -> cropped 4200x414 -> NV12 4096x404, stride 4096x404

The data and ownership flow is:

    [GStreamer BGR buffer]
            |
            v
    [DVPP input memory, H2D if needed]
            |
            v
    [PicDesc: BGR888, 12720 stride, 514 height stride]
            |
            +--> [RoiConfig: full or preview inclusive ROI]
            |
            v
    [acldvppVpcCropResizeAsync on stream[i]]
            |
            v
    [PicDesc: NV12 4096 stride, target height]
            |
            v
    [pending job owns descriptors, buffers, ROI until sync]
            |
            +--> [H.264 or H.265 encoder]
            |
            v
    [EOS drains jobs before memory/channel teardown]

If descriptor validation, allocation, ACL submission, synchronization, or
encoder negotiation fails, the element returns an error. It never invokes
`videocrop`, `videoscale`, or a CPU conversion fallback.

## Plan of Work

First, add format-aware stride and geometry helpers, the four crop properties,
and validation in `set_info` and job submission. The validation must check the
visible crop rectangle, inclusive CANN ROI parity, width/height stride bounds,
and output geometry before any device allocation.

Next, extend each pending VPC job with a CANN ROI descriptor. Create the ROI
from the element properties at submission time, call
`acldvppVpcCropResizeAsync`, and destroy the ROI only after the job stream has
been synchronized or the job has been safely discarded after stream cleanup.
The mock backend will retain its host-only behavior but must exercise the same
property and negotiation paths.

Then add tests for property defaults and validation, mock crop parsing, the
4240x514 BGR-to-NV12 geometry, both encoder branches, the concurrent tee, and
an injectable parser test that registers the actual plugin before calling
`gst_parse_launch` on the publishing pipeline string.

Finally, run the complete build/test gates, run the two finite pipelines and
the tee pipeline on the physical target, record commands and results in the
hardware matrix and both READMEs, run hooks, and commit only the intended
files.

## Concrete Steps

Run from `/workspace/VisionSDK/gstreamer-ascend-dvpp`.

    source /usr/local/Ascend/cann-9.1.0-beta.1/set_env.sh
    meson setup /tmp/gst-ascend-dvpp-crop-resize-mock -Dwith_ascend=false -Dtests=true
    meson compile -C /tmp/gst-ascend-dvpp-crop-resize-mock
    meson test -C /tmp/gst-ascend-dvpp-crop-resize-mock --print-errorlogs

    meson setup /tmp/gst-ascend-dvpp-crop-resize-cann \
      -Dwith_ascend=true -Dtests=true \
      -Dascend_prefix=/usr/local/Ascend/cann-9.1.0-beta.1
    meson compile -C /tmp/gst-ascend-dvpp-crop-resize-cann
    meson test -C /tmp/gst-ascend-dvpp-crop-resize-cann --print-errorlogs

The hardware pipelines use `device-id=0`, finite frame counts, and caps that
make the two requested geometries observable before the encoder. The actual
commands and EOS results are recorded in `docs/hardware-matrix.md`.

## Validation and Acceptance

The mock build and tests must compile and pass without CANN. The CANN build
must compile and link all test targets. The physical acceptance requires both
single-path pipelines to reach EOS, a tee pipeline to reach EOS on both H.264
and H.265 branches concurrently, and a parser utility to accept the final
publishing pipeline after registering the built plugin.

The hardware evidence must include the CANN version, product, NPU/device ID,
input format and geometry, visible and stride geometry, crop ROI, output caps,
encoder type, command, EOS result, and any failure logs. No device reset,
driver change, CANN upgrade, or termination of unrelated NPU processes is
allowed.

## Idempotence and Recovery

Build directories under `/tmp` may be removed and recreated. Runtime tests are
finite and use only the selected device. If a hardware operation fails, retain
the command and error output in the hardware matrix, fix only the plugin or
test issue, and rerun the smallest reproducer. Do not reset the NPU or kill
other processes to recover.

Before committing, run `git diff --check`, inspect the staged path list, run
the relevant mock and CANN tests, and use the repository's commit-message
writer with a `CHANGES:` section.

## Artifacts and Notes

The primary artifacts are the converter source and tests, `DESIGN.md`,
`README.md`, `README.zh-CN.md`, `docs/hardware-matrix.md`, this ExecPlan, and
the parser/integration helper if needed. Hardware logs must be concise and
reproducible; the acceptance claim is finite pipeline completion and negotiated
geometry, not decoded image quality.

## Interfaces and Dependencies

`ascendvideoconvert` keeps the public properties `device-id`,
`inflight-depth`, and `stream-count`, and adds four unsigned properties:
`crop-left`, `crop-right`, `crop-top`, and `crop-bottom`. The properties are
pixel counts removed from the corresponding input edges. Output width and
height remain negotiated through `video/x-raw` caps.

The hardware path uses CANN `acldvppVpcCropResizeAsync`,
`acldvppCreateRoiConfig`, `acldvppDestroyRoiConfig`, ACL picture descriptors,
and the existing stream and allocator helpers. Standard DVPP allocations are
released through `acldvppFree`; only ACL runtime allocations use their own
matching release functions. The test and build dependencies remain GStreamer
1.20+, GLib, Meson, Ninja, and CANN 9.1.0-beta.1 on the validated target.

Change note: added 2026-10-13 after privileged hardware detection and CANN
header inspection; this plan records the geometry, ROI parity, and ownership
decisions before implementing wide-input offload.

Change note: updated 2026-10-13 after hardware validation; direct 4240-wide
BGR was replaced with device-side two-tile VPC orchestration, and all mock and
CANN acceptance tests passed.
