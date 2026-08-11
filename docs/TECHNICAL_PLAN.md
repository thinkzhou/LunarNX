# LunarNX — 技术方案文档 (v0.1 实现后更新)

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

### 已完成 (v0.1)

| 模块 | 状态 | 说明 |
|------|------|------|
| 认证 | ✅ 完成 | MSAL Device Code Flow → RPS → XSTS → GSSV，含 Token 刷新 |
| API 客户端 | ✅ 完成 | Xbox REST API 完整封装 (Console 列表 / Session / SDP / ICE) |
| WebRTC | ✅ 完成 | libpeer 集成，SDP/ICE 交换，4 个 DataChannel (chat/control/input/message) |
| 视频解码 | ✅ 完成 | FFmpeg NVDEC H.264 硬件解码 (AV_PIX_FMT_NVTEGRA) |
| 视频渲染 | ✅ 完成 | deko3d NV12→RGB BT.709 shader，零拷贝 NVDEC→GPU，主线程提交 |
| 音频解码 | ✅ 完成 | FFmpeg Opus 软件解码 (48kHz 立体声 S16) |
| 音频输出 | ✅ 完成 | libnx audout DMA 环形缓冲区 |
| 手柄输入 | ✅ 完成 | Switch HID → Xbox 按键物理位置映射 + XInput 编码 |
| HD 震动 | ✅ 完成 | 4-motor rumble 协议解析，50% 强度缩放 |
| 音画同步 | ✅ 完成 | 基于时间戳的帧调度 (>5ms 丢帧, >2ms sleep) |
| UI | ✅ 完成 | borealis 3 页面 (Auth → Console List → Stream + Perf Overlay) |
| 稳定性 | ✅ 完成 | Keepalive 轮询，指数退避自动重连 (max 5 次)，Token 自动刷新 |
| 构建系统 | ✅ 完成 | Makefile.switch + CMake (desktop) 双目标 |

### PlayStation 当前边界 (v0.2 开发中)

- ✅ PSN OAuth 登录、refresh token 恢复和 PS5 device list 已在 Ryubing 验证；
- ✅ HTTP 401 会强制刷新 token 并重试一次；
- 🚧 LAN Pair 凭据改为 server MAC 主键，正在修复持久化和主机注入；
- 🚧 PSN Remote 正在对齐 chiaki-ng：control hole 后由 ChiakiSession 完成动态 registration 和 data hole；
- 📝 真机 `native_switch` 的可靠性策略仍待对齐 Akira：当前不会继承 Ryubing 专用的 4 次
  holepunch 重试、退避与 relay 兼容逻辑；模拟器链路跑通后需单独审计原生重试条件、
  port guessing 默认值和 PS5 唤醒时序，且真机始终不得经过 Mac relay；
- 🚧 PlayStation 页面将重做为 Account / My Consoles / Local Network，并使用 Pair、Connect、Wake & Connect 的真实状态；
- ⛔ 当前 PSN 目录成功不代表 NAT 打洞、媒体、输入或真机兼容已经完成。

- [ ] xCloud 支持
- [ ] 键盘输入 (Swkbd / USB HID)
- [ ] RCAS 锐化 / FSR 超分
- [ ] 设置页面 (码率/编解码器)
- [ ] 自适应码率
- [ ] 局域网 mDNS 主机发现

---

## 2. 代码结构

```
LunarNX/
├── src/
│   ├── main.cpp                       # 入口点，borealis Application 初始化
│   ├── common.h                       # 平台宏、路径常量、兼容类型
│   │
│   ├── auth/                          # 认证模块
│   │   ├── auth_manager.cpp/h         # 认证状态机 (Device Code → Poll → Xbox tokens)
│   │   ├── token_store.cpp/h          # Token 持久化到 SD 卡 JSON
│   │   └── xbox_signing.cpp/h         # ECDSA P-256 签名 (mbedtls/OpenSSL)
│   │
│   ├── api/                           # Xbox REST API 模块
│   │   ├── xbox_api_client.cpp/h      # Xbox API 封装 (Console/Session/SDP/ICE)
│   │   ├── http_client.cpp/h          # libcurl C++ 封装
│   │   └── api_constants.h            # URL/常量 (inline constexpr)
│   │
│   ├── webrtc/                        # WebRTC 模块
│   │   └── peer_manager.cpp/h         # libpeer 封装 (Offer/Answer, ICE, DataChannel, Rumble)
│   │
│   ├── stream/                        # 流处理模块
│   │   ├── video_decoder.cpp/h        # FFmpeg NVDEC H.264 硬解
│   │   ├── audio_decoder.cpp/h        # FFmpeg Opus 软解
│   │   ├── video_renderer.cpp/h       # deko3d NV12→RGB 渲染 (descriptor set + BT.709)
│   │   ├── audio_player.cpp/h         # libnx audout 音频输出 (环形缓冲)
│   │   ├── av_sync.cpp/h              # 音视频同步
│   │   └── perf_stats.h              # 性能统计 (FPS/解码延迟/丢包)
│   │
│   ├── input/                         # 输入模块
│   │   ├── gamepad_reader.cpp/h       # libnx HID 手柄读取
│   │   ├── xinput_encoder.cpp/h       # Switch→Xbox 按键映射 + XInput wire format
│   │   └── rumble_controller.cpp/h    # HD 震动 (4-motor rumble)
│   │
│   ├── app/                           # 应用层
│   │   └── stream_controller.cpp/h    # 串流生命周期管理 (Auth→Connect→Stream→Stop)
│   │
│   └── ui/                            # 界面模块
│       ├── auth_activity.cpp/h        # 认证页面 (显示设备码)
│       ├── main_activity.cpp/h        # 主机列表 + 分辨率选择
│       ├── stream_view.cpp/h          # 串流画面 + 状态/性能覆盖层
│       └── perf_overlay.cpp/h         # 半透明性能统计叠加层
│
├── lib/
│   ├── libpeer/                       # sepfy/libpeer (WebRTC C 实现)
│   ├── borealis/                      # natinusala/borealis (Switch UI 框架)
│   ├── switch/                        # Switch 预编译库 (FFmpeg, curl)
│   └── stubs/                         # 平台兼容桩 (mbedtls_timing, dav1d)
│
├── shaders/                           # deko3d shader 源码
│   ├── basic_vsh.glsl                 # 全屏四边形顶点着色器
│   └── texture_fsh.glsl               # NV12→RGB 片段着色器 (BT.709)
│
├── romfs/shaders/                     # 编译后的 dksh shader (嵌入 NRO)
│   ├── basic_vsh.dksh
│   └── texture_fsh.dksh
│
├── Makefile.switch                    # Switch 交叉编译 Makefile
└── CMakeLists.txt                     # Desktop 编译
```

> 注：实际代码结构已精简。认证模块移除了未使用的 device_code/embedded_login/sisu_auth 独立类（功能已整合进 auth_manager）；WebRTC 模块移除了 sdp_handler/ice_handler/session_manager/data_channel 独立类（功能已整合进 peer_manager + stream_controller）；UI 模块移除了占位 main_menu/remote_play_view/xcloud_view（由 main_activity/stream_view 替代）。

---

## 3. 核心依赖

### 3.1 直接依赖

| 库 | 版本/来源 | 许可证 | 用途 |
|------|---------|------|------|
| [libpeer](https://github.com/sepfy/libpeer) | git submodule | MIT | 轻量 WebRTC C 实现 (~1MB binary) |
| [averne/FFmpeg](https://github.com/averne/FFmpeg) | 预编译 .a | LGPL/GPL | H.264 NVDEC 硬解 + Opus 解码 |
| [borealis](https://github.com/natinusala/borealis) | git submodule | MIT | Switch 风格 C++ UI 框架 |
| [deko3d](https://github.com/devkitPro/deko3d) | devkitPro | Zlib | Switch 原生 GPU API |
| [libnx](https://github.com/switchbrew/libnx) | devkitPro | ISC | Switch 硬件 API (HID, audout, fs) |
| [libcurl](https://curl.se) | devkitPro switch-curl | MIT | HTTP 客户端 |
| [mbedtls](https://github.com/Mbed-TLS/mbedtls) | libpeer 自带 + 自编译 | Apache 2.0 | DTLS/SRTP + ECDSA P-256 签名 |
| [libsrtp](https://github.com/cisco/libsrtp) | libpeer 自带 + 自编译 | BSD-3 | SRTP 媒体加密 |
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

## 4. 协议参考

### Xbox GameStream 协议栈

LunarNX 实现了完整的 Xbox Remote Play 协议，从认证到串流：

```
认证: MSAL Device Code → RPS auth → XSTS (GSSV + web) → GSSV token
API:  GET /lists/devices → POST /v5/sessions/home/play → poll state
SDP:  POST/GET /v5/sessions/home/{id}/sdp (含 chat/control/input/message channel 配置)
ICE:  POST/GET /v5/sessions/home/{id}/ice
媒体: WebRTC H.264 (NVDEC) + Opus → DTLS-SRTP 加密
数据: SCTP DataChannel (SID 0-3): chat, control, input, message
输入: XInput wire format (38 bytes/frame) via input channel
震动: FourMotorRumble protocol (0x80 report type)
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
- libnx audout DMA 异步输出，环形缓冲区 (64KB) + mutex 线程安全
- 环绕写入正确处理 split 写入

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

LunarNX 在上述验证基础上独立实现。
