# gstreamer-ascend-dvpp / gst-ascend-dvpp

Hardware-accelerated video conversion, JPEG processing and codec integration
for Ascend Atlas inference products, powered by CANN ACL DVPP.

DVPP provides dedicated VPC, JPEGD, JPEGE, VDEC and VENC services. This plugin
exposes them as standard GStreamer elements and uses `acldvppMalloc` for device
media buffers on hardware builds.

> **Status:** experimental. The current target reports product `IT21PD2G10` and
> has been tested with CANN 9.1.0-beta.1. The physical validation covers VPC,
> JPEGD/JPEGE, four-frame H.264/H.265 encode, and four-access-unit H.264/H.265
> decode. Broader codec profiles and rate-control combinations remain
> unverified.

## Table of Contents

- [gstreamer-ascend-dvpp / gst-ascend-dvpp](#gstreamer-ascend-dvpp--gst-ascend-dvpp)
  - [Table of Contents](#table-of-contents)
  - [Features](#features)
  - [Requirements](#requirements)
  - [Building & Installation](#building--installation)
  - [Quick Start](#quick-start)
  - [Advanced Usage](#advanced-usage)
    - [`device-id` Property](#device-id-property)
    - [Crop Properties](#crop-properties)
    - [Wide Input Crop, Resize, and Encode](#wide-input-crop-resize-and-encode)
    - [`inflight-depth` Property](#inflight-depth-property)
    - [`stream-count` Property](#stream-count-property)
    - [NPU and DVPP Monitoring](#npu-and-dvpp-monitoring)
    - [CPU vs DVPP Multi-Stream Test](#cpu-vs-dvpp-multi-stream-test)
    - [Measured Occupancy](#measured-occupancy)
    - [Optimization Progress](#optimization-progress)
    - [Host-only Mock Backend](#host-only-mock-backend)
  - [Best Practice](#best-practice)
  - [Troubleshooting](#troubleshooting)
  - [Acknowledgements](#acknowledgements)
  - [License](#license)

## Features

- **VPC conversion and resize** through `ascendvideoconvert`.
- **JPEG hardware acceleration** through `ascendjpegdec` and `ascendjpegenc`.
- **H.264/H.265 VDEC and VENC elements** through `ascendh264dec`,
  `ascendh265dec`, `ascendh264enc` and `ascendh265enc`.
- **AscendDVPP allocator** for device media memory with explicit host-device
  copy fallback for ordinary GStreamer system-memory buffers.
- **Shared ACL runtime** with per-thread device activation for GStreamer
  streaming threads.
- **Mock backend** for plugin registration, allocator tests and caps
  negotiation without CANN.

## Requirements

| component | minimum version | note |
| --- | --- | --- |
| GStreamer | 1.20+ | `gstreamer-1.0`, `gstreamer-base-1.0`, `gstreamer-video-1.0`, and `gstreamer-allocators-1.0` development files |
| GLib | 2.0 development files | required by the plugin and tests |
| Build tools | C compiler, `pkg-config`, Meson, Ninja | required for compilation |
| CANN | 9.x recommended | ACL DVPP headers and libraries |
| Ascend product | Atlas inference series | first validated target is Ascend 310P3 |
| Linux device nodes | `/dev/davinci*`, `/dev/devmm_svm` | required for hardware execution |

The hardware build expects CANN under a prefix containing:

    aarch64-linux/include/acl/ops/acl_dvpp.h
    aarch64-linux/lib64/libacl_dvpp.so
    aarch64-linux/lib64/libascendcl.so

The direct CANN link dependencies are `libacl_dvpp.so` and `libascendcl.so`.
Source the CANN `set_env.sh` before building or running so their transitive
runtime libraries are on `LD_LIBRARY_PATH`. The mock build requires the
GStreamer and GLib development packages but does not require CANN or an
Ascend device.

## Building & Installation

```bash
# 1. Enter the repository
$ cd gstreamer-ascend-dvpp

# 2. Load the CANN runtime environment on an Ascend target
$ source /usr/local/Ascend/cann-9.1.0-beta.1/set_env.sh

# 3. Configure a hardware build
$ meson setup build \
    -Dwith_ascend=true \
    -Dascend_prefix=/usr/local/Ascend/cann-9.1.0-beta.1

# 4. Compile and test
$ ninja -C build
$ meson test -C build --print-errorlogs

# 5. Install
$ sudo ninja -C build install

# 6. Point GStreamer at the installed plugin when needed
$ export GST_PLUGIN_PATH_1_0="/usr/local/lib/$(uname -m)-linux-gnu/gstreamer-1.0"

# 7. Clear the GStreamer registry cache and inspect the plugin
$ rm -f /tmp/gst-ascend-registry.bin
$ GST_REGISTRY_1_0=/tmp/gst-ascend-registry.bin \
    gst-inspect-1.0 ascenddvpp
```

The plugin should report these seven factories:

    ascendvideoconvert
    ascendjpegdec / ascendjpegenc
    ascendh264dec / ascendh264enc
    ascendh265dec / ascendh265enc

## Quick Start

```bash
# NV12 640x480 -> NV12 320x240 through Ascend VPC
$ gst-launch-1.0 -e videotestsrc num-buffers=30 ! \
    video/x-raw,format=NV12,width=640,height=480,framerate=30/1 ! \
    ascendvideoconvert device-id=0 ! \
    video/x-raw,format=NV12,width=320,height=240,framerate=30/1 ! \
    fakesink sync=false

# NV12 -> JPEGD/JPEGE -> NV12 hardware round trip
$ gst-launch-1.0 -e videotestsrc num-buffers=30 ! \
    video/x-raw,format=NV12,width=320,height=240 ! \
    ascendjpegenc ! ascendjpegdec ! \
    video/x-raw,format=NV12,width=320,height=240 ! fakesink sync=false

# H.264 access-unit decode and encode
$ gst-launch-1.0 -e filesrc location=input.h264 ! h264parse ! \
    ascendh264dec ! ascendh264enc ! h264parse ! \
    filesink location=output.h264
```

VDEC and VENC require complete access units, supported codec profiles and a
product that exposes the requested DVPP capability. They do not silently fall
back to CPU codecs.

## Advanced Usage

### `device-id` Property

All hardware elements expose `device-id`. Use it when the process must select a
specific Ascend device:

```bash
gst-launch-1.0 videotestsrc num-buffers=30 ! \
  video/x-raw,format=NV12,width=640,height=480 ! \
  ascendvideoconvert device-id=1 ! fakesink
```

### Crop Properties

`ascendvideoconvert` exposes four unsigned crop properties. They are pixel
counts removed from the corresponding input edge:

| property | meaning |
| --- | --- |
| `crop-left` | remove pixels from the left edge |
| `crop-right` | remove pixels from the right edge |
| `crop-top` | remove pixels from the top edge |
| `crop-bottom` | remove pixels from the bottom edge |

The output width and height are selected with the negotiated `video/x-raw`
caps. The converter validates DVPP stride and ROI parity and reports an error
for unsupported geometry; it does not fall back to CPU crop or scaling.

### Wide Input Crop, Resize, and Encode

For a CPU-generated `4240x514` BGR frame, use DVPP for both branches:

```bash
# Main: BGR 4240x514 -> NV12 4096x496 -> H.265
gst-launch-1.0 -e videotestsrc num-buffers=1 pattern=black ! \
    video/x-raw,format=BGR,width=4240,height=514,framerate=1/1 ! \
    ascendvideoconvert device-id=0 ! \
    video/x-raw,format=NV12,width=4096,height=496 ! \
    ascendh265enc ! h265parse ! filesink location=main.h265

# Preview: crop 20/20/50/50, then NV12 4096x404 -> H.264
gst-launch-1.0 -e videotestsrc num-buffers=1 pattern=black ! \
    video/x-raw,format=BGR,width=4240,height=514,framerate=1/1 ! \
    ascendvideoconvert device-id=0 crop-left=20 crop-right=20 \
      crop-top=50 crop-bottom=50 ! \
    video/x-raw,format=NV12,width=4096,height=404 ! \
    ascendh264enc ! h264parse ! filesink location=preview.h264
```

On Ascend 310P, a packed BGR input wider than 4096 is split into device-memory
tiles before VPC. This follows the product's >4096 input restrictions; the
tile copies and crop/resize/paste operations remain on the device. Do not
insert CPU `videocrop` or `videoscale` into these branches. See
[`docs/hardware-matrix.md`](docs/hardware-matrix.md) for the exact strides,
ROIs, failure evidence for the rejected single-request path, and hardware
EOS results.

### `inflight-depth` Property

`ascendvideoconvert` submits several VPC jobs before retiring the oldest
output. The default depth is 4:

```bash
gst-launch-1.0 -e videotestsrc is-live=true ! \
    video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! \
    ascendvideoconvert device-id=0 inflight-depth=8 ! \
    video/x-raw,format=NV12,width=640,height=360 ! fakesink sync=false
```

Jobs are retired in input order. The output pool keeps `2 * inflight-depth + 4`
buffers so a downstream converter can retain upstream outputs while both
queues are in flight. Flush and EOS drain or discard pending jobs before the
ACL stream and DVPP memory are destroyed.

### `stream-count` Property

`stream-count` selects the number of ACL streams used by the in-flight queue.
The default is 2 and the supported range is 1-4. Each submitted job keeps its
stream until retirement, so H2D copies and VPC requests can overlap across
streams. Increase this value only after checking `npu-smi`; it improves
submission concurrency but does not increase the hardware VPC limit.

### NPU and DVPP Monitoring

On the current target, monitor device 0 through the NPU management tool:

```bash
watch -n 1 npu-smi info -t usages -i 64 -c 0
```

The output includes `DVPP VPC`, `DVPP JPEGD`, `DVPP JPEGE`, `DVPP VDEC` and
`DVPP VENC` usage counters. The current validation target is product
`IT21PD2G10` with two NPU devices exposed through `/dev/davinci0` and
`/dev/davinci1`.

### CPU vs DVPP Multi-Stream Test

The repository includes `tests/benchmark_cpu_vs_dvpp.sh`. It decodes one H.264
file, fans the decoded NV12 frames into multiple branches, and compares CPU
`videoscale`/`videoconvert` against `ascendvideoconvert`. Both paths produce
640x480 BGR. The decode and common NV12 preparation stay outside the comparison:

```bash
# Prepare an input file with any available H.264 encoder.
gst-launch-1.0 -e videotestsrc num-buffers=600 ! \
    video/x-raw,format=I420,width=1920,height=1080,framerate=30/1 ! \
    openh264enc bitrate=12000000 ! h264parse ! filesink location=test.h264

# CPU conversion baseline.
BRANCHES=6 DURATION=20 tests/benchmark_cpu_vs_dvpp.sh cpu test.h264

# Ascend DVPP conversion.
source /usr/local/Ascend/cann-9.1.0-beta.1/set_env.sh
export GST_PLUGIN_PATH_1_0="$PWD/build-ascend"
BRANCHES=6 DURATION=20 tests/benchmark_cpu_vs_dvpp.sh dvpp test.h264
```

Set `GST_SHARK_CPUUSAGE=1` when the optional `cpuusage` tracer is installed.
Without it, the script samples the `gst-launch-1.0` process with `ps` and
always samples device counters with `npu-smi`.

### Measured Occupancy

The following measurements were collected on the current Atlas 310P3 with
CANN 9.1.0-beta.1. `npu-smi` was sampled every 0.2 seconds with
`npu-smi info -t usages -i 64 -c 0`.

| workload | test load | observed occupancy |
| --- | --- | --- |
| VPC sustained load | 16 concurrent branches, 1920x1080 NV12 -> 640x360 NV12, time-limited stress run | DVPP VPC 17-18% after preroll; DDR 3%; AICPU 3-5%; CtrlCPU 1-6% in the stable phase |
| Upstream-DVPP VPC load | 8 concurrent branches, 1920x1080 NV12@60 -> 640x360 NV12, `inflight-depth=8`, `stream-count=1/2` | DVPP VPC stable at 5%; DDR 3-4%; AICPU 1-2%; CtrlCPU 1-5% after warm-up |
| CPU vs DVPP conversion | 6 branches, one 600-frame 1920x1080 H.264 input, NV12 -> 640x480 BGR | CPU baseline: host process 417%; DVPP: host process 100%, DVPP VPC 1.5%, CtrlCPU 4% |
| JPEGD/JPEGE | 320x240 NV12@30 -> JPEGE -> JPEGD -> NV12, 300 frames in 9.97 seconds | DVPP JPEGE peak 1%; DVPP JPEGD peak 1%; DDR 3% |

The VPC sustained-load run was intentionally time-limited and is an occupancy
measurement, not a complete EOS throughput benchmark. The DVPP columns are
instantaneous integer-percent readings and should be interpreted together with
the pipeline result and sampling interval.

### Optimization Progress

The current branch contains three incremental optimizations:

1. DVPP-backed output allocation. A converter output can be allocated by the
   `AscendDVPP` allocator, and a following converter uses its device pointer
   directly. The converter also proposes that allocator upstream, so an
   allocator-aware source can produce DVPP memory at the source pad.
2. In-flight VPC submission. `inflight-depth` enqueues asynchronous H2D/VPC
   work and retires frames in order. A hardware test with depth 4, 640x480
   NV12 -> 320x240 NV12 and 30 frames completed EOS successfully.
3. Stream and runtime overhead reduction. `stream-count` distributes jobs over
   up to four ACL streams, while a thread-local runtime cache avoids repeating
   `aclrtSetDevice` on the same active runtime generation.

Large system-memory inputs still allocate temporary DVPP input buffers per
   frame when the upstream element does not accept the allocation query. On
   the current target, `videotestsrc` accepted the proposal and the converter
   debug path reported `input-device=1 output-device=1`.

The upstream-DVPP stress run used 8 branches at 1920x1080 NV12@60 -> 640x360
NV12 with `inflight-depth=8`. Both `stream-count=1` and `stream-count=2`
reported a stable DVPP VPC value of 5% (DDR 3-4%, AICPU 1-2%). The equivalent
30 fps run was stable at 2%. These are sampled occupancy values, not a
hardware saturation benchmark.

The CPU-vs-DVPP run reduced the measured host GStreamer process CPU from 417%
to 100%, an observed reduction of about 76%. The DVPP branch moved the resize
and NV12-to-BGR conversion to VPC; decode and the common NV12 preparation
remained on the host, so the result measures conversion offload rather than an
end-to-end hardware decode pipeline.

### Host-only Mock Backend

The mock backend validates the GStreamer ABI, plugin registration, allocator
behavior and caps negotiation without accessing CANN or Ascend devices:

```bash
$ meson setup build-mock -Dwith_ascend=false -Dtests=true
$ ninja -C build-mock
$ meson test -C build-mock --print-errorlogs
$ GST_PLUGIN_PATH_1_0="$PWD/build-mock" \
    GST_REGISTRY_1_0=/tmp/gst-ascend-mock-registry.bin \
    gst-inspect-1.0 ascenddvpp
```

The mock backend does not validate hardware output quality or performance.

## Best Practice

1. Use DVPP buffers for media inputs and outputs whenever the upstream and
   downstream elements can share device memory.
2. Keep `queue` elements between independent decode, conversion and encode
   stages when building multi-stream pipelines.
3. Keep input and output dimensions within the selected product's VPC/JPEG/VENC
   limits and account for DVPP stride alignment.
4. Use complete access units for VDEC and avoid assuming that arbitrary byte
   chunks are decodable frames.

## Troubleshooting

| symptom | cause | remedy |
| --- | --- | --- |
| `No such element ascendvideoconvert` | plugin was not found | set `GST_PLUGIN_PATH_1_0` and clear the GStreamer registry cache |
| `libacl_dvpp_mpi.so: cannot open shared object file` | CANN environment was not loaded | source the CANN `set_env.sh` before running GStreamer |
| `ctx is NULL` or `acldvppMalloc failed` | streaming thread has no ACL device context | use the current source with per-thread device activation and verify `device-id` |
| `not negotiated` | caps or dimensions are unsupported | specify NV12/raw caps explicitly and check product limits |
| VDEC/VENC callback timeout | the target stream is incomplete, unsupported, or the device report worker cannot process it | use complete access units and verify the selected product's codec support; the current target has only been validated with the H.264/H.265 smoke streams described in `docs/hardware-matrix.md` |
| `/dev/davinci*` is missing | Ascend driver or container device mapping is unavailable | run on a target with the Ascend driver and device nodes exposed |

## Acknowledgements

- [Ascend CANN documentation](https://www.hiascend.com/document/detail/en/canncommercial/850/API/appdevgapi/aclpythondevg_01_0411.html) - DVPP API and product constraints.
- [Ascend samples](https://github.com/ascend/samples) - reference ACL and media-processing flows.
- [gstreamer-rga](https://github.com/corenel/gstreamer-rga) - GStreamer hardware converter structure and project documentation style.

## License

MIT - see [LICENSE](LICENSE).
