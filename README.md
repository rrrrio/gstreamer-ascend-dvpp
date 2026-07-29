# gstreamer-ascend-dvpp / gst-ascend-dvpp

Hardware-accelerated video conversion, JPEG processing and codec integration
for Ascend Atlas inference products, powered by CANN ACL DVPP.

DVPP provides dedicated VPC, JPEGD, JPEGE, VDEC and VENC services. This plugin
exposes them as standard GStreamer elements and uses `acldvppMalloc` for device
media buffers on hardware builds.

> **Status:** experimental. VPC and JPEGD/JPEGE have been smoke-tested on an
> Atlas 310P3 with CANN 9.1.0-beta.1. VDEC/VENC are registered and compiled but
> still need the CANN report-processing loop and complete target codec streams.

## Table of Contents

- [gstreamer-ascend-dvpp / gst-ascend-dvpp](#gstreamer-ascend-dvpp--gst-ascend-dvpp)
  - [Table of Contents](#table-of-contents)
  - [Features](#features)
  - [Requirements](#requirements)
  - [Building & Installation](#building--installation)
  - [Quick Start](#quick-start)
  - [Advanced Usage](#advanced-usage)
    - [`device-id` Property](#device-id-property)
    - [NPU and DVPP Monitoring](#npu-and-dvpp-monitoring)
    - [Measured Occupancy](#measured-occupancy)
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
| GStreamer | 1.20 | `gstreamer-1.0`, base and video libraries |
| Meson / Ninja | current distribution versions | build system |
| CANN | 9.x recommended | provides ACL DVPP headers and libraries |
| Ascend product | Atlas inference series | first validated target is Ascend 310P3 |
| Linux device nodes | `/dev/davinci*`, `/dev/devmm_svm` | required for hardware execution |

The hardware build expects CANN under a prefix containing:

    aarch64-linux/include/acl/ops/acl_dvpp.h
    aarch64-linux/lib64/libacl_dvpp.so
    aarch64-linux/lib64/libascendcl.so

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

### NPU and DVPP Monitoring

On the current target, monitor device 0 through the NPU management tool:

```bash
watch -n 1 npu-smi info -t usages -i 64 -c 0
```

The output includes `DVPP VPC`, `DVPP JPEGD`, `DVPP JPEGE`, `DVPP VDEC` and
`DVPP VENC` usage counters.

### Measured Occupancy

The following measurements were collected on the current Atlas 310P3 with
CANN 9.1.0-beta.1. `npu-smi` was sampled every 0.2 seconds with
`npu-smi info -t usages -i 64 -c 0`.

| workload | test load | observed occupancy |
| --- | --- | --- |
| VPC sustained load | 16 concurrent branches, 1920x1080 NV12 -> 640x360 NV12, time-limited stress run | DVPP VPC 17-18% after preroll; DDR 3%; AICPU 3-5%; CtrlCPU 1-6% in the stable phase |
| JPEGD/JPEGE | 320x240 NV12@30 -> JPEGE -> JPEGD -> NV12, 300 frames in 9.97 seconds | DVPP JPEGE peak 1%; DVPP JPEGD peak 1%; DDR 3% |

The VPC sustained-load run was intentionally time-limited and is an occupancy
measurement, not a complete EOS throughput benchmark. The current element
implementation synchronizes each VPC request and copies every frame through
host memory, so this result is an implementation stress result rather than a
hardware saturation result. The DVPP columns are instantaneous integer-percent
readings and should be interpreted together with the pipeline result and
sampling interval.

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
| VDEC/VENC callback timeout | CANN report processing is not running or input is incomplete | use complete access units; VDEC/VENC report-loop integration is still pending in this version |
| `/dev/davinci*` is missing | Ascend driver or container device mapping is unavailable | run on a target with the Ascend driver and device nodes exposed |

## Acknowledgements

- [Ascend CANN documentation](https://www.hiascend.com/document/detail/en/canncommercial/850/API/appdevgapi/aclpythondevg_01_0411.html) - DVPP API and product constraints.
- [Ascend samples](https://github.com/ascend/samples) - reference ACL and media-processing flows.
- [gstreamer-rga](https://github.com/corenel/gstreamer-rga) - GStreamer hardware converter structure and project documentation style.

## License

MIT - see [LICENSE](LICENSE).
