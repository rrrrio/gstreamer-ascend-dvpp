# Hardware Matrix

This repository uses the ACL DVPP C API from CANN. The API family contains
separate VPC, JPEGD, JPEGE, VDEC, and VENC services. A pipeline must satisfy
both GStreamer caps and the selected product's DVPP restrictions.

| Capability | Plugin element | First baseline | Important constraints |
| --- | --- | --- | --- |
| VPC resize/CSC | `ascendvideoconvert` | Atlas inference / Ascend 310P | DVPP stride alignment and supported pixel formats apply. |
| JPEG decode | `ascendjpegdec` | Atlas inference / Ascend 310P | Output is NV12 in the first implementation. |
| JPEG encode | `ascendjpegenc` | Atlas inference / Ascend 310P | Input is NV12; output buffer size is predicted by DVPP. |
| H.264 decode | `ascendh264dec` | Atlas inference / Ascend 310P | Input must be complete access units; callback report loop still needs target-stream validation. |
| H.265 decode | `ascendh265dec` | Atlas inference / Ascend 310P | Input must be complete access units; callback report loop still needs target-stream validation. |
| H.264 encode | `ascendh264enc` | Product-dependent | Check VENC support and resolution limits; callback report loop still needs target-stream validation. |
| H.265 encode | `ascendh265enc` | Product-dependent | Check VENC support and rate-control limits; callback report loop still needs target-stream validation. |

The implementation keeps device allocations alive until an asynchronous DVPP
callback has completed. JPEG and VPC are hardware-tested on the current 310P3
target. VDEC and VENC currently contain the callback and ownership plumbing,
but need the CANN report-processing thread (`aclrtSubscribeReport` plus
`aclrtProcessReport`) and complete target codec streams before being called
hardware-validated.

The official CANN documentation states that VPC/JPEGD/JPEGE/VDEC/VENC are
separate DVPP functions, that VPC image buffers should use DVPP allocation, and
that VDEC/VENC completion is callback-driven. See:

- [Media Data Processing V2 overview](https://www.hiascend.com/document/detail/en/canncommercial/850/API/appdevgapi/aclpythondevg_01_0411.html)
- [VPC restrictions](https://www.hiascend.com/document/detail/en/canncommercial/850/API/appdevgapi/aclcppdevg_03_0337.html)
- [VDEC API sequence](https://www.hiascend.com/document/detail/en/canncommercial/800/appdevg/acldevg/aclcppdevg_000055.html)
- [VENC API sequence](https://www.hiascend.com/document/detail/en/canncommercial/850/appdevg/acldevg/aclcppdevg_000056.html)
