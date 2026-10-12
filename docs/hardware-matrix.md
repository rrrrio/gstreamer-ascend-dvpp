# Hardware Matrix

This repository uses the ACL DVPP C API from CANN. The API family contains
separate VPC, JPEGD, JPEGE, VDEC, and VENC services. A pipeline must satisfy
both GStreamer caps and the selected product's DVPP restrictions.

| Capability | Plugin element | First baseline | Important constraints |
| --- | --- | --- | --- |
| VPC resize/CSC | `ascendvideoconvert` | `IT21PD2G10` | Hardware-tested with chained NV12 reuse and the wide-input crop/resize paths below; DVPP stride, ROI, and product format restrictions apply. |
| JPEG decode | `ascendjpegdec` | `IT21PD2G10` | Hardware-tested in a 320x240 NV12 round trip; output is NV12 in the first implementation. |
| JPEG encode | `ascendjpegenc` | `IT21PD2G10` | Hardware-tested in a 320x240 NV12 round trip; input is NV12 and output buffer size is predicted by DVPP. |
| H.264 decode | `ascendh264dec` | `IT21PD2G10` | Hardware smoke-tested with four 320x240 Annex-B access units; input must be complete access units and EOS drain is required. |
| H.265 decode | `ascendh265dec` | `IT21PD2G10` | Hardware smoke-tested with four 320x240 Annex-B access units produced by the matching encoder; input must be complete access units and EOS drain is required. |
| H.264 encode | `ascendh264enc` | `IT21PD2G10` | Hardware smoke-tested with four 320x240 NV12 frames; check VENC profile and rate-control limits for other streams. |
| H.265 encode | `ascendh265enc` | `IT21PD2G10` | Hardware smoke-tested with four 320x240 NV12 frames; output was independently identified as H.265/HEVC. Check VENC profile and rate-control limits for other streams. |

## Wide-Input Validation

The validation target is CANN `9.1.0-beta.1`, product `IT21PD2G10`, NPU ID
`64`, plugin `device-id=0`, and `/dev/davinci0`, `/dev/davinci1`, and
`/dev/devmm_svm` exposed to the process. The device nodes were readable and
writable by the granted runtime identity; the earlier "no hardware" result was
therefore a permissions issue, not a missing device.

The installed DVPP declarations require a width stride between 32 and 16384
bytes, a height stride between 6 and 4096 lines, and inclusive ROI coordinates
with even left/top and odd right/bottom. The requested buffers have these
descriptors:

| branch | input visible geometry | input stride | ROI | cropped source | output |
| --- | --- | --- | --- | --- | --- |
| main H.265 | BGR 4240x514 | 12720x514 | (0,4239,0,513) | 4240x514 | NV12 4096x496, stride 4096x496 |
| preview H.264 | BGR 4240x514 | 12720x514 | (20,4219,50,463) | 4200x414 | NV12 4096x404, stride 4096x404 |

The stride limits alone do not make the direct request valid on 310P. A source
wider than 4096 uses the product's 8K VPC restrictions, whose supported input
format list does not include BGR888. The first direct implementation submitted
the complete 4240-wide BGR picture and failed on this target with:

```text
acldvppVpcCropResizeAsync failed: ret=507018, recent=E39999: Inner Error!
Kernel task ... aicpu exception ... acldvppVpcCropResizeAsync
```

The converter now handles packed input wider than 4096 with a device-only two
tile path. It copies each source row with ACL device-to-device copies into BGR
tile buffers no wider than 4096, then submits two
`acldvppVpcCropResizePasteAsync` requests into disjoint ranges of the shared
NV12 output. There is no CPU `videocrop`, `videoscale`, or silent fallback.
The product-specific limits are documented in the [CANN VPC restrictions]
documentation and the [CANN crop-resize API] documentation.

[CANN VPC restrictions]: https://www.hiascend.com/document/detail/en/canncommercial/850/API/appdevgapi/aclcppdevg_03_0337.html
[CANN crop-resize API]: https://www.hiascend.com/document/detail/en/canncommercial/850/API/appdevgapi/aclcppdevg_03_0341.html

## Reproducible Commands

From the repository root on the validation target:

```bash
source /usr/local/Ascend/cann-9.1.0-beta.1/set_env.sh

# Main: 4240x514 BGR -> 4096x496 NV12 -> H.265
/tmp/gst-ascend-dvpp-crop-resize-cann/test-ascend-crop-resize \
    -p /ascend-crop-resize/full-frame-h265

# Preview: crop 20/20/50/50 -> 4096x404 NV12 -> H.264
/tmp/gst-ascend-dvpp-crop-resize-cann/test-ascend-crop-resize \
    -p /ascend-crop-resize/cropped-h264

# One input frame, both branches, concurrent H.264 and H.265 EOS
/tmp/gst-ascend-dvpp-crop-resize-cann/test-ascend-crop-resize \
    -p /ascend-crop-resize/concurrent-codec

# Complete mock/CANN build and test gate
meson test -C /tmp/gst-ascend-dvpp-crop-resize-cann --print-errorlogs
```

Results on 2026-10-13: `full-frame-h265` passed, `cropped-h264` passed,
`concurrent-codec` passed, and the complete CANN suite passed `5/5`. The suite
also parses the publishing pipeline with the registered converter and encoder
types through `gst_parse_launch`; set `ASCEND_PUBLISH_PIPELINE` to inject the
deployment's exact string into `/ascend-crop-resize/publishing-pipeline-parse`.

The implementation keeps device allocations alive until an asynchronous DVPP
callback has completed. JPEG and VPC are hardware-tested on the current
`IT21PD2G10` target. H.264 and H.265 VDEC/VENC have smoke-tested complete
access-unit pipelines, including report processing and teardown; broader codec
matrices remain unverified. Each codec element creates a report worker, binds
its OS thread ID through the VDEC/VENC channel description, and calls
`aclrtProcessReport`. VENC additionally creates and subscribes a dedicated ACL
report stream; VDEC uses the channel's report path.

The hardware device-memory reuse probe uses 128x96 -> 64x48 -> 32x24 NV12
stages. A 32x24 -> 16x12 second stage was rejected by the current device, so
sub-32 dimensions remain outside the validated matrix until product-specific
DVPP limits are confirmed.

The official CANN documentation states that VPC/JPEGD/JPEGE/VDEC/VENC are
separate DVPP functions, that VPC image buffers should use DVPP allocation, and
that VDEC/VENC completion is callback-driven. See:

- [Media Data Processing V2 overview](https://www.hiascend.com/document/detail/en/canncommercial/850/API/appdevgapi/aclpythondevg_01_0411.html)
- [VPC restrictions](https://www.hiascend.com/document/detail/en/canncommercial/850/API/appdevgapi/aclcppdevg_03_0337.html)
- [VDEC API sequence](https://www.hiascend.com/document/detail/en/canncommercial/800/appdevg/acldevg/aclcppdevg_000055.html)
- [VENC API sequence](https://www.hiascend.com/document/detail/en/canncommercial/850/appdevg/acldevg/aclcppdevg_000056.html)
