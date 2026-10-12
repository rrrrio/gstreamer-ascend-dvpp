# Hardware Matrix

This repository uses the ACL DVPP C API from CANN. The API family contains
separate VPC, JPEGD, JPEGE, VDEC, and VENC services. A pipeline must satisfy
both GStreamer caps and the selected product's DVPP restrictions.

| Capability | Plugin element | First baseline | Important constraints |
| --- | --- | --- | --- |
| VPC resize/CSC | `ascendvideoconvert` | `IT21PD2G10` | Hardware-tested with chained 128x96 -> 64x48 -> 32x24 NV12 reuse; DVPP stride alignment and supported pixel formats apply. |
| JPEG decode | `ascendjpegdec` | `IT21PD2G10` | Hardware-tested in a 320x240 NV12 round trip; output is NV12 in the first implementation. |
| JPEG encode | `ascendjpegenc` | `IT21PD2G10` | Hardware-tested in a 320x240 NV12 round trip; input is NV12 and output buffer size is predicted by DVPP. |
| H.264 decode | `ascendh264dec` | `IT21PD2G10` | Hardware smoke-tested with four 320x240 Annex-B access units; input must be complete access units and EOS drain is required. |
| H.265 decode | `ascendh265dec` | `IT21PD2G10` | Hardware smoke-tested with four 320x240 Annex-B access units produced by the matching encoder; input must be complete access units and EOS drain is required. |
| H.264 encode | `ascendh264enc` | `IT21PD2G10` | Hardware smoke-tested with four 320x240 NV12 frames; check VENC profile and rate-control limits for other streams. |
| H.265 encode | `ascendh265enc` | `IT21PD2G10` | Hardware smoke-tested with four 320x240 NV12 frames; output was independently identified as H.265/HEVC. Check VENC profile and rate-control limits for other streams. |

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
