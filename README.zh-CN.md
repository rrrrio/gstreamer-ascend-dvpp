# gstreamer-ascend-dvpp / gst-ascend-dvpp

这是面向昇腾 Atlas 推理系列产品的 GStreamer 硬件加速插件，底层使用
CANN ACL DVPP，提供视频格式转换、缩放、JPEG 处理以及 H.264/H.265 编解码
能力。

DVPP 包含独立的 VPC、JPEGD、JPEGE、VDEC 和 VENC 服务。本插件将这些服务
封装为标准 GStreamer element，并在硬件构建中使用 `acldvppMalloc` 申请媒体
设备内存。

> **状态：** 实验性。当前设备产品标识为 `IT21PD2G10`，使用 CANN
> 9.1.0-beta.1。已完成 VPC、JPEGD/JPEGE、4 帧 H.264/H.265 编码，以及各自
> 四个 access unit 的 H.264/H.265 解码硬件验证。更完整的 codec profile/码率
> 组合仍未验证。

## 目录

- [gstreamer-ascend-dvpp / gst-ascend-dvpp](#gstreamer-ascend-dvpp--gst-ascend-dvpp)
  - [目录](#目录)
  - [特性](#特性)
  - [环境要求](#环境要求)
  - [编译与安装](#编译与安装)
  - [快速体验](#快速体验)
  - [高级用法](#高级用法)
    - [`device-id` 属性](#device-id-属性)
    - [裁剪属性](#裁剪属性)
    - [宽输入裁剪缩放与编码](#宽输入裁剪缩放与编码)
    - [`inflight-depth` 属性](#inflight-depth-属性)
    - [`stream-count` 属性](#stream-count-属性)
    - [NPU 与 DVPP 监控](#npu-与-dvpp-监控)
    - [CPU 与 DVPP 多路测试](#cpu-与-dvpp-多路测试)
    - [实测性能占用](#实测性能占用)
    - [优化过程](#优化过程)
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
| GStreamer | 1.20+ | `gstreamer-1.0`、`gstreamer-base-1.0`、`gstreamer-video-1.0`、`gstreamer-allocators-1.0` 开发文件 |
| GLib | 2.0 开发文件 | 插件和测试必需 |
| 构建工具 | C 编译器、`pkg-config`、Meson、Ninja | 编译必需 |
| CANN | 推荐 9.x | 提供 ACL DVPP 头文件和库 |
| 昇腾产品 | Atlas 推理系列 | 首个验证目标为 Ascend 310P3 |
| Linux 设备节点 | `/dev/davinci*`、`/dev/devmm_svm` | 硬件执行必需 |

硬件构建要求 CANN 前缀下存在：

    aarch64-linux/include/acl/ops/acl_dvpp.h
    aarch64-linux/lib64/libacl_dvpp.so
    aarch64-linux/lib64/libascendcl.so

项目直接链接的 CANN 库是 `libacl_dvpp.so` 和 `libascendcl.so`。编译和
运行前都要执行 CANN 的 `set_env.sh`，由它把这些库的传递依赖加入
`LD_LIBRARY_PATH`。Mock 构建只需要 GStreamer 和 GLib 开发包，不需要 CANN
或昇腾设备。

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

### 裁剪属性

`ascendvideoconvert` 提供四个无符号裁剪属性，值表示从对应输入边缘
移除的像素数：

| 属性 | 含义 |
| --- | --- |
| `crop-left` | 从左边缘移除像素 |
| `crop-right` | 从右边缘移除像素 |
| `crop-top` | 从上边缘移除像素 |
| `crop-bottom` | 从下边缘移除像素 |

输出宽高通过协商得到的 `video/x-raw` caps 指定。转换器会验证 DVPP stride
和 ROI 奇偶约束；不支持的几何尺寸会报错，不会静默退回 CPU 裁剪或缩放。

### 宽输入裁剪缩放与编码

对于 CPU 生成的 `4240x514` BGR 测试帧，两路都使用 DVPP：

```bash
# 主路：BGR 4240x514 -> NV12 4096x496 -> H.265
gst-launch-1.0 -e videotestsrc num-buffers=1 pattern=black ! \
    video/x-raw,format=BGR,width=4240,height=514,framerate=1/1 ! \
    ascendvideoconvert device-id=0 ! \
    video/x-raw,format=NV12,width=4096,height=496 ! \
    ascendh265enc ! h265parse ! filesink location=main.h265

# 预览路：左右各裁 20、上下各裁 50，再 NV12 4096x404 -> H.264
gst-launch-1.0 -e videotestsrc num-buffers=1 pattern=black ! \
    video/x-raw,format=BGR,width=4240,height=514,framerate=1/1 ! \
    ascendvideoconvert device-id=0 crop-left=20 crop-right=20 \
      crop-top=50 crop-bottom=50 ! \
    video/x-raw,format=NV12,width=4096,height=404 ! \
    ascendh264enc ! h264parse ! filesink location=preview.h264
```

在 Ascend 310P 上，宽度超过 4096 的 packed BGR 输入会先在设备内存中
分成多个 tile，再提交 VPC。这是产品对大尺寸输入的限制所要求的路径；
分块拷贝以及裁剪/缩放/粘贴仍在设备上完成。不要在这些分支插入 CPU
`videocrop` 或 `videoscale`。具体 stride、ROI、单次请求失败记录和硬件
EOS 结果见
[`docs/hardware-matrix.md`](docs/hardware-matrix.md)。

### `inflight-depth` 属性

`ascendvideoconvert` 会先提交多个 VPC job，再回收最早提交的输出。默认
depth 为 4：

```bash
gst-launch-1.0 -e videotestsrc is-live=true ! \
    video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! \
    ascendvideoconvert device-id=0 inflight-depth=8 ! \
    video/x-raw,format=NV12,width=640,height=360 ! fakesink sync=false
```

job 按输入顺序回收。输出 pool 保留 `2 * inflight-depth + 4` 个 buffer，允许
下游 converter 在两级队列同时运行时继续持有上游输出。Flush 和 EOS 会在
销毁 ACL stream 及 DVPP memory 之前排空或丢弃 pending job。

### `stream-count` 属性

`stream-count` 控制 in-flight 队列使用的 ACL stream 数量，默认值为 2，
范围为 1-4。每个 job 在回收前固定使用其提交时选择的 stream，因此多个
stream 可以重叠执行 H2D copy 和 VPC 请求。调大该值前应结合 `npu-smi`
检查；它只增加提交并发度，不会突破硬件 VPC 上限。

### NPU 与 DVPP 监控

当前设备可以使用以下命令监控 device 0：

```bash
watch -n 1 npu-smi info -t usages -i 64 -c 0
```

输出中包括 `DVPP VPC`、`DVPP JPEGD`、`DVPP JPEGE`、`DVPP VDEC` 和
`DVPP VENC` 使用率。当前验证设备产品标识为 `IT21PD2G10`，并暴露
`/dev/davinci0`、`/dev/davinci1` 两个设备节点。

### CPU 与 DVPP 多路测试

仓库提供 `tests/benchmark_cpu_vs_dvpp.sh`。脚本只解码一次 H.264，再将
NV12 帧分发到多路，比较 CPU 的 `videoscale`/`videoconvert` 与
`ascendvideoconvert`，两侧都输出 640x480 BGR。解码和公共的 NV12 预处理
不计入两种转换方式的差异：

```bash
# 使用当前可用的 H.264 encoder 生成测试文件。
gst-launch-1.0 -e videotestsrc num-buffers=600 ! \
    video/x-raw,format=I420,width=1920,height=1080,framerate=30/1 ! \
    openh264enc bitrate=12000000 ! h264parse ! filesink location=test.h264

# CPU 软件转换基线。
BRANCHES=6 DURATION=20 tests/benchmark_cpu_vs_dvpp.sh cpu test.h264

# Ascend DVPP 转换。
source /usr/local/Ascend/cann-9.1.0-beta.1/set_env.sh
export GST_PLUGIN_PATH_1_0="$PWD/build-ascend"
BRANCHES=6 DURATION=20 tests/benchmark_cpu_vs_dvpp.sh dvpp test.h264
```

安装了可选的 `cpuusage` tracer 时可以设置 `GST_SHARK_CPUUSAGE=1`。当前
设备没有安装 gst-shark，脚本会回退到 `ps` 采样 `gst-launch-1.0` 进程，
设备计数器仍始终通过 `npu-smi` 采集。

### 实测性能占用

以下数据在当前 Atlas 310P3、CANN 9.1.0-beta.1 上采集。使用
`npu-smi info -t usages -i 64 -c 0`，采样间隔为 0.2 秒。

| 负载 | 测试负载 | 实测占用 |
| --- | --- | --- |
| VPC 持续负载 | 16 路并发，1920x1080 NV12 -> 640x360 NV12，定时压测 | preroll 后 DVPP VPC 为 17-18%；DDR 3%；AICPU 3-5%；稳定阶段 CtrlCPU 1-6% |
| 上游-DVPP VPC 负载 | 8 路并发，1920x1080 NV12@60 -> 640x360 NV12，`inflight-depth=8`，`stream-count=1/2` | DVPP VPC 稳定为 5%；DDR 3-4%；AICPU 1-2%；预热后 CtrlCPU 1-5% |
| CPU 与 DVPP 转换 | 6 路，一个 600 帧 1920x1080 H.264 输入，NV12 -> 640x480 BGR | CPU 基线：宿主进程 417%；DVPP：宿主进程 100%，DVPP VPC 1.5%，CtrlCPU 4% |
| JPEGD/JPEGE | 320x240 NV12@30 -> JPEGE -> JPEGD -> NV12，300 帧耗时 9.97 秒 | DVPP JPEGE 峰值 1%；DVPP JPEGD 峰值 1%；DDR 3% |

VPC 持续负载测试是为观察设备占用而设置的定时压测，不是完整 EOS 吞吐
基准。DVPP 列是瞬时整数百分比读数，需要结合 pipeline 结果和采样间隔理解。

### 优化过程

当前分支包含三项增量优化：

1. DVPP-backed output allocation：转换器输出可以由 `AscendDVPP` allocator
   分配，后续转换器直接使用其 device pointer。转换器还会向上游提出该
   allocator，使支持 allocation query 的 source 可以直接产出 DVPP memory。
2. 多帧 in-flight VPC 提交：`inflight-depth` 异步排入 H2D/VPC，并按顺序
   回收帧。硬件上使用 depth 4、640x480 NV12 -> 320x240 NV12、30 帧的测试
   已成功 EOS。
3. Stream 和 runtime 开销优化：`stream-count` 在最多 4 条 ACL stream 之间
   分配 job；线程级 runtime cache 在同一 runtime 代次内避免重复调用
   `aclrtSetDevice`。

当上游不接受 allocation query 时，大分辨率 system-memory 输入仍会逐帧
申请临时 DVPP input buffer。当前设备上 `videotestsrc` 已接受该提议，
converter debug 报告为 `input-device=1 output-device=1`。

上游-DVPP 压测使用 8 路、1920x1080 NV12@60 -> 640x360 NV12、
`inflight-depth=8`。`stream-count=1` 和 `stream-count=2` 的 DVPP VPC 都稳定为
5%（DDR 3-4%，AICPU 1-2%）；同一负载降到 30 fps 时稳定为 2%。这些是采样到的
占用率，不是硬件满载基准。

CPU 与 DVPP 对比中，宿主 GStreamer 进程 CPU 从 417% 降到 100%，实测下降
约 76%。DVPP 分支将缩放和 NV12 到 BGR 的转换交给 VPC；解码和公共 NV12
预处理仍在宿主侧，因此该结果表示转换卸载效果，不是完整的硬件解码端到端
结果。

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
| VDEC/VENC callback timeout | 目标码流不完整、不受支持，或设备 report worker 无法处理 | 使用完整 access unit 并确认目标产品的 codec 能力；当前设备只验证了 `docs/hardware-matrix.md` 中的 H.264/H.265 冒烟码流 |
| 缺少 `/dev/davinci*` | 昇腾驱动或容器设备映射不可用 | 在加载昇腾驱动且映射设备节点的目标环境运行 |

## 鸣谢

- [昇腾 CANN 文档](https://www.hiascend.com/document/detail/en/canncommercial/850/API/appdevgapi/aclpythondevg_01_0411.html)：DVPP API 和产品限制。
- [Ascend samples](https://github.com/ascend/samples)：ACL 和媒体处理参考流程。
- [gstreamer-rga](https://github.com/corenel/gstreamer-rga)：GStreamer 硬件转换插件结构和项目文档风格参考。

## 许可证

MIT - 详见 [LICENSE](LICENSE)。
