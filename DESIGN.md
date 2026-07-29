# Design

The plugin is split by GStreamer base class while sharing one ACL runtime and
one memory abstraction. VPC and JPEG operations are frame transforms. VDEC and
VENC are asynchronous codec elements whose callback completes a frame-owned
wait context.

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
                                  [callback / sync]
                                         |
                                         v
                              [GStreamer output buffer]
```

The mock backend replaces device allocation and ACL calls with host allocation
and copies. It is intentionally limited to ABI and negotiation validation.
