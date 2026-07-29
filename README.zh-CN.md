# gstreamer-ascend-dvpp / gst-ascend-dvpp

这是面向昇腾 Atlas 推理系列产品的 GStreamer 硬件加速插件，底层使用
CANN ACL DVPP，提供视频格式转换、缩放、JPEG 处理以及 H.264/H.265 编解码
能力。

DVPP 包含独立的 VPC、JPEGD、JPEGE、VDEC 和 VENC 服务。本插件将这些服务
封装为标准 GStreamer element，并在硬件构建中使用 `acldvppMalloc` 申请媒体
设备内存。

> **状态：** 实验性。VPC 和 JPEGD/JPEGE 已在 Atlas 310P3、CANN
> 9.1.0-beta.1 上完成基础验证。VDEC/VENC 已完成编译和 element 注册，但仍
> 需要补充 CANN report 处理线程以及完整目标码流验证。

## 目录

- [gstreamer-ascend-dvpp / gst-ascend-dvpp](#gstreamer-ascend-dvpp--gst-ascend-dvpp)
  - [目录](#目录)
  - [特性](#特性)
  - [环境要求](#环境要求)
  - [编译与安装](#编译与安装)
  - [快速体验](#快速体验)
  - [高级用法](#高级用法)
    - [`device-id` 属性](#device-id-属性)
    - [NPU 与 DVPP 监控](#npu-与-dvpp-监控)
    - [实测性能占用](#实测性能占用)
    - [主机 Mock 后端](#主机-mock-后端)
  - [最佳实践](#最佳实践)
  - [故障排除](#故障排除)
  - [鸣谢](#鸣谢)
  - [许可证](#许可证)

## 特性

- **VPC 格式转换与缩放**：由 `ascendvideoconvert` 提供。
- **JPEG 硬件加速**：由 `ascendjpegdec` 和 `ascendjpegenc` 提供。
- **H.264/H.265 VDEC/VENC element**：提供 `ascendh264dec`、
  `ascendh265dec`、`ascendh264enc` 和 `ascendh265enc`。
- **AscendDVPP allocator**：申请设备媒体内存，并为普通 GStreamer
  system-memory buffer 提供显式 Host/Device 拷贝路径。
- **共享 ACL runtime**：针对 GStreamer streaming thread 进行 device 激活。
- **Mock 后端**：不依赖 CANN 即可验证插件注册、allocator、单元测试和 caps
  协商。

## 环境要求

| 组件 | 最低版本 | 说明 |
| --- | --- | --- |
| GStreamer | 1.20 | 需要 `gstreamer-1.0`、base 和 video 库 |
| Meson / Ninja | 当前发行版版本 | 构建系统 |
| CANN | 推荐 9.x | 提供 ACL DVPP 头文件和库 |
| 昇腾产品 | Atlas 推理系列 | 首个验证目标为 Ascend 310P3 |
| Linux 设备节点 | `/dev/davinci*`、`/dev/devmm_svm` | 硬件执行必需 |

硬件构建要求 CANN 前缀下存在：

    aarch64-linux/include/acl/ops/acl_dvpp.h
    aarch64-linux/lib64/libacl_dvpp.so
    aarch64-linux/lib64/libascendcl.so

## 编译与安装

```bash
# 进入仓库
$ cd gstreamer-ascend-dvpp

# 在昇腾目标机上加载 CANN 环境
$ source /usr/local/Ascend/cann-9.1.0-beta.1/set_env.sh

# 配置硬件构建
$ meson setup build \
    -Dwith_ascend=true \
    -Dascend_prefix=/usr/local/Ascend/cann-9.1.0-beta.1

# 编译和测试
$ ninja -C build
$ meson test -C build --print-errorlogs

# 安装
$ sudo ninja -C build install

# 必要时设置插件搜索路径
$ export GST_PLUGIN_PATH_1_0="/usr/local/lib/$(uname -m)-linux-gnu/gstreamer-1.0"

# 清理 GStreamer registry 并检查插件
$ rm -f /tmp/gst-ascend-registry.bin
$ GST_REGISTRY_1_0=/tmp/gst-ascend-registry.bin \
    gst-inspect-1.0 ascenddvpp
```

插件应注册以下 7 个 factory：

    ascendvideoconvert
    ascendjpegdec / ascendjpegenc
    ascendh264dec / ascendh264enc
    ascendh265dec / ascendh265enc

## 快速体验

```bash
# Ascend VPC：NV12 640x480 -> NV12 320x240
$ gst-launch-1.0 -e videotestsrc num-buffers=30 ! \
    video/x-raw,format=NV12,width=640,height=480,framerate=30/1 ! \
    ascendvideoconvert device-id=0 ! \
    video/x-raw,format=NV12,width=320,height=240,framerate=30/1 ! \
    fakesink sync=false

# 硬件 JPEG 编码再解码：NV12 -> JPEGD/JPEGE -> NV12
$ gst-launch-1.0 -e videotestsrc num-buffers=30 ! \
    video/x-raw,format=NV12,width=320,height=240 ! \
    ascendjpegenc ! ascendjpegdec ! \
    video/x-raw,format=NV12,width=320,height=240 ! fakesink sync=false

# H.264 access unit 解码和编码
$ gst-launch-1.0 -e filesrc location=input.h264 ! h264parse ! \
    ascendh264dec ! ascendh264enc ! h264parse ! \
    filesink location=output.h264
```

VDEC/VENC 要求输入为完整 access unit，并且目标昇腾产品支持对应 codec
profile 和 DVPP 能力。插件不会静默降级到 CPU 编解码器。

## 高级用法

### `device-id` 属性

所有硬件 element 都提供 `device-id` 属性。多卡场景下可以显式选择设备：

```bash
gst-launch-1.0 videotestsrc num-buffers=30 ! \
  video/x-raw,format=NV12,width=640,height=480 ! \
  ascendvideoconvert device-id=1 ! fakesink
```

### NPU 与 DVPP 监控

当前设备可以使用以下命令监控 device 0：

```bash
watch -n 1 npu-smi info -t usages -i 64 -c 0
```

输出中包括 `DVPP VPC`、`DVPP JPEGD`、`DVPP JPEGE`、`DVPP VDEC` 和
`DVPP VENC` 使用率。

### 实测性能占用

以下数据在当前 Atlas 310P3、CANN 9.1.0-beta.1 上采集。使用
`npu-smi info -t usages -i 64 -c 0`，采样间隔为 0.2 秒。

| 负载 | 测试负载 | 实测占用 |
| --- | --- | --- |
| VPC 持续负载 | 16 路并发，1920x1080 NV12 -> 640x360 NV12，定时压测 | preroll 后 DVPP VPC 为 17-18%；DDR 3%；AICPU 3-5%；稳定阶段 CtrlCPU 1-6% |
| JPEGD/JPEGE | 320x240 NV12@30 -> JPEGE -> JPEGD -> NV12，300 帧耗时 9.97 秒 | DVPP JPEGE 峰值 1%；DVPP JPEGD 峰值 1%；DDR 3% |

VPC 持续负载测试是为观察设备占用而设置的定时压测，不是完整 EOS 吞吐
基准。当前 element 实现会对每次 VPC 请求同步等待，并通过 Host 内存完成
每帧拷贝，因此该结果是当前实现的压力测试结果，不代表硬件已经满载。DVPP
列是瞬时整数百分比读数，需要结合 pipeline 结果和采样间隔理解。

### 主机 Mock 后端

Mock 后端不访问 CANN 和昇腾设备，可用于验证 GStreamer ABI、插件注册、
allocator 和 caps 协商：

```bash
$ meson setup build-mock -Dwith_ascend=false -Dtests=true
$ ninja -C build-mock
$ meson test -C build-mock --print-errorlogs
$ GST_PLUGIN_PATH_1_0="$PWD/build-mock" \
    GST_REGISTRY_1_0=/tmp/gst-ascend-mock-registry.bin \
    gst-inspect-1.0 ascenddvpp
```

Mock 后端不代表真实 DVPP 的输出质量或性能。

## 最佳实践

1. 上下游支持时优先复用 DVPP 设备内存，减少 Host/Device 拷贝。
2. 多路 pipeline 中，在解码、转换和编码等独立阶段之间加入 `queue`。
3. 根据目标产品的 VPC/JPEG/VENC 限制选择分辨率，并预留 DVPP stride 对齐。
4. VDEC 输入必须是完整 access unit，不能把任意字节块当作可解码帧。

## 故障排除

| 现象 | 原因 | 解决方案 |
| --- | --- | --- |
| `No such element ascendvideoconvert` | 插件未被找到 | 设置 `GST_PLUGIN_PATH_1_0` 并清理 GStreamer registry |
| `libacl_dvpp_mpi.so: cannot open shared object file` | 未加载 CANN 环境 | 运行 GStreamer 前先执行 CANN 的 `set_env.sh` |
| `ctx is NULL` 或 `acldvppMalloc failed` | streaming thread 没有 ACL device context | 使用当前版本的线程 device 激活逻辑并检查 `device-id` |
| `not negotiated` | caps 或尺寸不支持 | 显式指定 NV12/raw caps，并检查目标产品限制 |
| VDEC/VENC callback timeout | 未运行 CANN report 处理，或输入码流不完整 | 使用完整 access unit；当前版本仍需补充 VDEC/VENC report loop |
| 缺少 `/dev/davinci*` | 昇腾驱动或容器设备映射不可用 | 在加载昇腾驱动且映射设备节点的目标环境运行 |

## 鸣谢

- [昇腾 CANN 文档](https://www.hiascend.com/document/detail/en/canncommercial/850/API/appdevgapi/aclpythondevg_01_0411.html)：DVPP API 和产品限制。
- [Ascend samples](https://github.com/ascend/samples)：ACL 和媒体处理参考流程。
- [gstreamer-rga](https://github.com/corenel/gstreamer-rga)：GStreamer 硬件转换插件结构和项目文档风格参考。

## 许可证

MIT - 详见 [LICENSE](LICENSE)。
