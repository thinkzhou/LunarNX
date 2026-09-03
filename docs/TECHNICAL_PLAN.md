# LunarNX — 当前技术架构与实现状态

> Xbox + PlayStation 双平台扩展的当前设计见
> [xbox_playstation_dual_streaming_design.md](xbox_playstation_dual_streaming_design.md)。
>
> PlayStation 页面、LAN Pair、PSN Remote、身份关联和连接状态机的详细契约见
> [PlayStation Page and Launch Flow Design](superpowers/specs/2026-07-31-playstation-page-and-launch-flow-design.md)。

> Nintendo Switch (Atmosphère) 上运行的 Xbox Remote Play 串流客户端
>
> 本文档记录 LunarNX 的技术架构和实现细节，随代码演进持续更新。

## 目录

1. [当前状态](#1-当前状态)
2. [代码结构](#2-代码结构)
3. [核心依赖](#3-核心依赖)
4. [协议参考](#4-协议参考)
5. [Switch 平台适配](#5-switch-平台适配)
6. [参考项目](#6-参考项目)

---

## 1. 当前状态

本节描述当前 `main`，不再沿用早期 v0.1/v0.2 的实施清单。“真机测试通过”表示该链路已经在真实 Nintendo Switch 上完成过端到端验证，不代表所有主机固件、账号地区、NAT 和网络环境都必然兼容。

| 模块 | 状态 | 当前实现 |
|------|------|---------|
| Xbox 认证与 API | ✅ 已实现 | Microsoft Device Code、RPS/XSTS/GSSV、Token 恢复与刷新、主机列表、Cloud 游戏库和 Session API |
| Xbox 主机 Remote Play | ✅ 真机通过 | 局域网和互联网连接均已在真实 Switch 上串流；互联网连接仍要求 NAT/防火墙允许建立兼容的直连路径 |
| Xbox Cloud Gaming | ✅ 真机通过 | 游戏库、地区选择、会话建立、音视频、输入和恢复链路均已验证 |
| Xbox WebRTC | ✅ 已实现 | legacy libpeer、SDP/ICE、DTLS-SRTP、usrsctp DataChannel、成功 STUN 与 Home ICE 路径复用 |
| PlayStation 本地主机 | ✅ 真机通过 | PS4/PS5 的发现、PIN 配对、凭据保存、唤醒和局域网串流均已验证 |
| PlayStation Network | ✅ 真机通过 | PSN OAuth、PS5 设备列表、动态注册、打洞和远程串流均已验证 |
| PlayStation 媒体与输入 | ✅ 已实现 | PS4/PS5 H.264、PS5 HEVC、Opus、按键、触摸板、体感、普通震动和 DualSense 触觉反馈 |
| 共享媒体管线 | ✅ 已实现 | 协议隔离的接收策略、NVDEC、deko3d 零拷贝呈现、Audren、音画同步和分辨率切换 |
| 网络适应与恢复 | ✅ 已实现 | 路径质量估计、自适应 REMB 码率、动态延迟模式、NACK/PLI、关键帧恢复和有界队列 |
| UI 与设置 | ✅ 已实现 | 平台首页、Xbox/PS 主机与游戏列表、画质/解码/锐化设置、独立按键映射、串流菜单和性能统计 |
| 生命周期 | ✅ 已实现 | 取消、Token 刷新、HOME 前后台恢复、控制链路重建、安全退出和可取消的网络等待 |
| 构建与发布 | ✅ 已实现 | Docker/devkitA64 Switch 构建、Desktop 测试目标、BSS 守卫、版本化 NRO/ZIP Release 和校验和 |

---

## 2. 代码结构

```text
LunarNX/
├── src/
│   ├── auth/       # Microsoft/Xbox 认证、签名和 Token 持久化
│   ├── api/        # HTTP、Xbox 主机/Cloud 目录和 Session API
│   ├── app/        # Xbox Profile、Session、SDP/ICE、DataChannel 和生命周期
│   ├── webrtc/     # legacy libpeer 封装、路径估计、RTP jitter 与 Xbox 反馈
│   ├── ps/         # PS4/PS5 发现、配对、PSN、Chiaki Session、输入和触觉反馈
│   ├── stream/     # 共享 H.264/HEVC/Opus 解码、同步、渲染、音频和统计
│   ├── input/      # Switch HID、Xbox XInput 编码和震动输出
│   ├── ui/         # Borealis 页面、列表、设置、串流画面和覆盖层
│   └── platform/   # Switch/desktop 平台适配与网络工作线程
├── tools/
│   ├── libpeer_legacy/  # 可复现的 legacy libpeer Switch 补丁链
│   ├── chiaki_switch/   # chiaki-ng Switch SDK 构建和补丁
│   └── ffmpeg_switch_build/ # FFmpeg/NVDEC Switch 构建
├── shaders/        # deko3d shader 源码
├── romfs/          # UI 资源、语言和编译后的 shader
├── Makefile.switch # Switch 交叉编译入口
└── Makefile.desktop / CMakeLists.txt # Desktop 构建和测试入口
```

---

## 3. 核心依赖

### 3.1 直接依赖

| 库 | 版本/来源 | 许可证 | 用途 |
|------|---------|------|------|
| [libpeer](https://github.com/sepfy/libpeer) | 固定 revision + LunarNX 补丁链 | MIT | Xbox WebRTC、ICE、DTLS-SRTP 和 DataChannel |
| [FFmpeg](https://github.com/FFmpeg/FFmpeg) | wiliwili/averne Switch NVDEC 补丁路线 | LGPL/GPL，取决于构建配置 | H.264/HEVC NVDEC 硬解和 Opus 解码 |
| [Borealis](https://github.com/XITRIX/borealis) | XITRIX fork 固定 revision + GPU 生命周期补丁 | MIT | Switch 风格 C++ UI 框架 |
| [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng) | 固定 revision + LunarNX Switch 补丁链 | AGPL-3.0 | PlayStation Remote Play 协议、PSN 连接和媒体传输 |
| [deko3d](https://github.com/devkitPro/deko3d) | devkitPro | Zlib | Switch 原生 GPU API |
| [libnx](https://github.com/switchbrew/libnx) | devkitPro | ISC | Switch 硬件 API (HID, audout, fs) |
| [curl](https://curl.se) | Moonlight Switch curl 8.x 构建 | curl license | Xbox/PS HTTP、HTTPS 和 PSN WSS 信令 |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | libpeer 与 Switch curl 构建使用 | Apache-2.0 或 GPL-2.0-or-later，取决于版本 | TLS、DTLS-SRTP 和 ECDSA P-256 签名 |
| [libsrtp](https://github.com/cisco/libsrtp) | libpeer 自带 + 自编译 | BSD-3 | SRTP 媒体加密 |
| [usrsctp](https://github.com/sctplab/usrsctp) | libpeer 自带 + Switch 兼容补丁 | BSD-3-Clause | Xbox SCTP DataChannel |
| [json-c](https://github.com/json-c/json-c) | Switch portlib | MIT | chiaki-ng/PSN JSON 支持 |
| [miniupnpc](https://github.com/miniupnp/miniupnp) | Switch portlib | BSD-3-Clause | PlayStation 网络连接支持 |
| [cJSON](https://github.com/DaveGamble/cJSON) | libpeer 自带 | MIT | JSON 解析 |

### 3.2 libpeer 传递依赖

```
libpeer
  ├── mbedtls    (DTLS/SRTP — 需自编译以启用 DTLS-SRTP 扩展)
  ├── libsrtp    (SRTP 媒体加密)
  ├── cJSON      (SDP/ICE 消息解析)
  └── coreHTTP   (信令 HTTP 请求 — 未使用, LunarNX 用 libcurl)
```

### 3.3 Switch 系统库

| 库 | 用途 |
|------|------|
| libnx HID | 手柄输入 (PadState, Buttons, Sticks) |
| libnx audout | 音频输出 (DMA ring buffer) |
| libnx arm | 系统时钟 (armGetSystemTick, mbedtls timing) |
| deko3d | GPU 渲染 (CmdBuf, Shader, Image, Queue) |

---

## 4. 协议路径

### Xbox GameStream 协议栈

LunarNX 实现了 Xbox 主机 Remote Play 和 Xbox Cloud Gaming 从认证到串流的完整客户端路径：

```
认证: MSAL Device Code → RPS auth → XSTS (GSSV + web) → GSSV token
API:  主机列表 / Cloud 游戏库 → Home 或 Cloud Session → poll state
SDP:  POST/GET Session SDP（含 chat/control/input/message channel 配置）
ICE:  POST/GET Session ICE → 候选排序与上次成功路径软复用
媒体: WebRTC H.264 (NVDEC) + Opus → DTLS-SRTP 加密
数据: SCTP DataChannel (SID 0-3): chat, control, input, message
输入: XInput wire format (38 bytes/frame) via input channel
震动: FourMotorRumble protocol (0x80 report type)
```

### PlayStation Remote Play 协议栈

```text
本地: UDP discovery → PIN registration → credential store → wake/connect
远程: PSN OAuth → PS5 device list → control/data hole punching → dynamic registration
会话: chiaki-ng Session → Takion encrypted media/control transport
媒体: H.264/HEVC (NVDEC) + Opus → shared MediaPipeline
输入: Switch HID → ChiakiControllerState + touchpad + motion
反馈: PlayStation rumble / DualSense haptics → Switch vibration
```

### 关键协议细节

- **Contract v4**: Xbox 的 SDP 包含 `reliableinput` (v9) 和 `unreliableinput` (v9) 两个额外的数据通道配置，匹配 libnxbox 的实现
- **XInput 线格式**: 14 字节头 (ReportType + Sequence + Timestamp) + 1 字节 GamepadFrameCount + 23 字节 GamepadFrame (index + button mask + 4 axes + 2 triggers + physicality)，匹配 XStreaming
- **Rumble**: ReportType 0x80 flag，10+ 字节 (type + gpIndex + 4 motors × 0-100 + duration/delay/repeat)，匹配 XStreaming
- **SDP 配置**: offer 中包含 4 个 channel 的 capability (chat/control/input/message) 和 2 个额外 channel (reliableinput/unreliableinput v9)，匹配 libnxbox

---

## 5. Switch 平台适配

### 5.1 deko3d 渲染

- **着色器**: RomFS 嵌入 (`romfs:/shaders/*.dksh`)，使用 GLSL 460 编译为 deko3d shader binary
- **纹理绑定**: 使用 descriptor set 模式。2 个 ImageDescriptor (R8 luma + RG8 chroma) + 2 个 SamplerDescriptor，通过 `dkCmdBufBindImageDescriptorSet` / `dkCmdBufBindSamplerDescriptorSet` 绑定，`dkMakeTextureHandle` 创建纹理句柄
- **零拷贝**: 通过 `av_nvtegra_frame_get_fbuf_map` 获取 NVDEC 输出缓冲的 NvMap 句柄，以 `DkMemBlockFlags_Image` + external storage 方式创建 deko3d 图像
- **线程安全**: `render()` 在 stream 线程构建 cmdlist，`present()` 通过 `brls::sync()` 在 borealis 主线程提交 deko3d queue（匹配 Moonlight-Switch 模式）
- **色彩矩阵**: BT.709 有限范围 YCbCr→RGB，pushConstants 传递转换矩阵

### 5.2 NVDEC 解码

- 通过 FFmpeg `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_NVTEGRA)` 初始化
- `pix_fmt = AV_PIX_FMT_NVTEGRA` 输出零拷贝硬件帧
- 单线程 (`thread_count = 1`)，NVDEC 是 Tegra X1 的独立解码引擎
- 接受损坏帧 (`AV_CODEC_FLAG_OUTPUT_CORRUPT`) 以处理流媒体丢包

### 5.3 音频

- Opus 软件解码 (FFmpeg)，CPU 开销极低
- libnx Audren 异步输出，按 Realtime、Balanced、Resilient 三档动态控制 60–100ms 缓冲容量
- Xbox 与 PlayStation 使用独立的入队和恢复策略，来源切换时保留安全的 voice 生命周期

### 5.4 震动

- `hidInitializeVibrationDevices` 初始化，支持 Handheld 和 Player 1 模式
- 50% 强度缩放 (`RUMBLE_SCALE = 0.5f`)，匹配 libnxbox 惯例
- 左握把: 160Hz 低频 (主马达) + 320Hz 高频 (扳机马达)
- 右握把: 160Hz 低频 (主马达) + 320Hz 高频 (扳机马达)

---

## 6. 参考项目

### 6.1 协议与架构参考

| 项目 | 说明 | 参考了什么 |
|------|------|-----------|
| [XStreaming](https://github.com/Geocld/XStreaming) | iOS/Android Xbox 串流客户端 | Xbox 协议完整文档、XInput 线格式、FourMotorRumble 协议、API 端点和 JSON 格式 |
| [PeaSyo](https://github.com/Geocld/PeaSyo) | Android PlayStation Remote Play 客户端 | PlayStation Remote Play 产品流程、连接行为和交互设计参考 |
| [libnxbox](https://github.com/ursusworks/libnxbox) | Switch Xbox 串流客户端 | libpeer + NVDEC + deko3d 技术路线验证、contract v4 channel 配置、50% rumble 缩放 |
| [Moonlight-Switch](https://github.com/XITRIX/Moonlight-Switch) | Switch NVIDIA GameStream 客户端 | deko3d 渲染模式 (DKVideoRenderer)、audout 音频输出、borealis UI 模式、FFmpeg NVDEC 配置、deko3d 线程模型 |
| [xbox-xcloud-player](https://github.com/unknownskl/xbox-xcloud-player) | JS xCloud WebRTC 库 | SDP 配置格式、Channel 声明、contract 版本号 |
| [Greenlight](https://github.com/unknownskl/greenlight) | Xbox 串流桌面客户端 | MSAL Device Code Flow 实现、GSSV token 请求格式 |

### 6.2 技术验证

libnxbox 已验证以下栈在 Switch 上可行：

- ✅ libpeer → WebRTC (SDP/ICE/DTLS-SRTP/SCTP)
- ✅ averne/FFmpeg → NVDEC H.264 硬解码 (720p, Very High preset)
- ✅ deko3d → NV12→RGB 渲染 (BT.709, 三重缓冲)
- ✅ libnx audout → Opus 48kHz Stereo 音频输出 (~60ms 延迟)
- ✅ HID → XInput wire format 手柄映射
- ✅ MSAL Device Code → Token 管理 → Xbox Auth

Moonlight-Switch 已验证以下模式在 Switch 上可行：

- ✅ deko3d descriptor set 纹理绑定
- ✅ FFmpeg NVDEC `av_nvtegra_frame_get_fbuf_map` 零拷贝
- ✅ borealis + deko3d 共享 DkQueue (主线程提交)
- ✅ audout DMA 环形缓冲区
- ✅ borealis Activity 导航模式

XStreaming 和 PeaSyo 是协议行为与客户端流程参考，并非 LunarNX 的运行时依赖。LunarNX 在上述开源项目和平台验证基础上实现自己的 Switch 应用层、生命周期与适配代码。
