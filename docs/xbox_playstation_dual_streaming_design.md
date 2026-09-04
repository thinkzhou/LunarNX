# LunarNX Xbox + PlayStation 双平台串流技术方案

状态：已实现；本文保留原始设计与分阶段计划作为架构记录
日期：2026-07-30  
目标版本：v0.2+  

> 当前状态（2026-09）：Xbox 主机 Remote Play、Xbox Cloud Gaming、PS4/PS5
> 局域网发现/配对/唤醒/串流，以及 PS5 通过 PSN 的远程串流均已在真实
> Nintendo Switch 硬件上完成端到端测试。当前实现状态以
> [TECHNICAL_PLAN.md](TECHNICAL_PLAN.md) 和项目 README 为准；下方 Phase
> 列表记录的是实施过程，不再代表待办清单。

## 1. 目标与约束

LunarNX 将在同一个 Nintendo Switch 应用中同时支持：

- Xbox 主机 Remote Play；
- Xbox Cloud Gaming（xCloud）；
- PlayStation 4/PlayStation 5 局域网 Remote Play；
- PlayStation 4/PlayStation 5 经 PSN 的异地 Remote Play。

Xbox 路径已经能稳定建立真实 xCloud 会话并显示音视频。本方案的首要原则是新增 PlayStation 旁路，不重写或大规模抽象已经稳定的 Xbox 协议实现。

必须继续遵守现有 Switch 工程约束：

- Switch 库和 NRO 只在 `devkitpro/devkita64:20251117` Docker 环境构建；
- 继续使用当前 legacy `lib/libpeer`，不切换 upstream provider；
- NRO BSS 保持低于 32 MiB；
- 网络失败不能阻止应用进入首页；
- 串流控制器只在用户进入对应功能后创建；
- 模拟器只用于回归，真实 Switch 是最终兼容性标准。

## 2. 已验证的技术基线

### 2.1 Xbox 基线

当前 Xbox 路径为：

```text
Microsoft/Xbox Auth
  -> Xbox REST Session API
  -> SDP/ICE
  -> libpeer WebRTC（DTLS-SRTP + SCTP）
  -> RTP H.264 / Opus
  -> MediaPipeline
  -> FFmpeg NVTEGRA + deko3d + audout
```

现有实现继续保留在：

- `src/auth/`、`src/api/`：Microsoft/Xbox 认证和 API；
- `src/app/xbox_*`、`src/app/web_rtc_transport.*`：Xbox 会话和 WebRTC；
- `src/webrtc/`：libpeer、RTP、DataChannel；
- `src/stream/`：共享媒体后端。

### 2.2 依赖升级验证

已经完成以下组合的 Switch 构建和 Ryubing 实流验证：

- 当前 LunarNX `main` 代码；
- 当前 active legacy libpeer；
- curl 8.11.0；
- mbedTLS 3.4.0；
- 真实 Xbox/xCloud 会话；
- H.264/Opus 接收、解码和实际画面显示。

验证结果：

- NRO BSS 为约 20.4 MiB；
- Xbox session、libpeer SCTP、DTLS read loop、DataChannel PPID 测试通过；
- curl/mbedTLS 升级没有破坏已验证的 Xbox 串流功能。

验证边界：

- 使用了 `CURL_VERIFY=0`，尚未验证 mbedTLS CA/hostname 校验；
- 尚未在真实 Switch 上验证升级后的依赖；
- chiaki-lib 已经交叉编译和链接，但还没有建立真实 PS 会话。

### 2.3 chiaki-lib Switch 探针

现有实验已经证明：

- `libchiaki.a` 可以为 Switch 构建；
- chiaki、Xbox 可以共享同一份 curl 8.11 和 mbedTLS 3.4；
- curl 需要同时启用 HTTP、HTTPS、WS、WSS；
- chiaki 依赖的 curl WebSocket API 可由该构建提供；
- chiaki-ng 1.10 的 mbedTLS 2.x 私有结构访问需要适配 mbedTLS 3。

实验说明和补丁目前位于实验 worktree 的 `tools/chiaki_switch/`。正式集成时应将可复现脚本、补丁和说明迁入主分支，而不是依赖实验目录中的生成产物。

## 3. 总体架构

双平台架构分为三层：

```text
┌──────────────────────── UI / Application ────────────────────────┐
│ Platform selector | console lists | loading | shared StreamView  │
└──────────────────────────────┬────────────────────────────────────┘
                               │ IStreamRuntime
              ┌────────────────┴────────────────┐
              │                                 │
┌─────────────▼─────────────┐       ┌───────────▼───────────────┐
│ Existing Xbox Controller │       │ New PlayStation Controller│
│ Xbox API + WebRTC        │       │ Discovery + PSN + Chiaki  │
└─────────────┬─────────────┘       └───────────┬───────────────┘
              │ encoded H.264/Opus              │ encoded H.264/HEVC/Opus
              └────────────────┬─────────────────┘
                               │
                    ┌──────────▼──────────┐
                    │ Shared MediaPipeline│
                    │ decode/sync/render  │
                    └─────────────────────┘
```

设计规则：

1. Xbox 和 PlayStation 不共享协议层抽象。
2. 两边只在“完整编码访问单元 + 时间戳”边界汇合。
3. Xbox 现有 `StreamController` 和 `XboxStreamSession` 行为保持不变。
4. 共用 UI 只依赖一个窄的运行时接口，不了解 WebRTC 或 Chiaki。
5. PlayStation 代码集中在 `src/ps/`，避免把 AGPL 协议实现散入 Xbox 模块。

## 4. 推荐代码结构

```text
src/
├── app/
│   ├── stream_runtime.h              # 共用串流页面需要的窄接口
│   ├── stream_controller.*           # 保留：Xbox controller
│   └── xbox_*.{h,cpp}                 # 保留：Xbox session/channel/API orchestration
├── ps/
│   ├── ps_stream_controller.{h,cpp}  # PS 生命周期和 UI facade
│   ├── ps_stream_session.{h,cpp}     # ChiakiSession 所有权和状态机
│   ├── ps_media_bridge.{h,cpp}       # Chiaki callback -> MediaPipeline
│   ├── ps_console_repository.{h,cpp} # 本地发现 + PSN 设备聚合
│   ├── ps_console_resolver.{h,cpp}   # 本地优先、远程回退
│   ├── ps_registration.{h,cpp}       # PIN 注册和凭据持久化
│   ├── psn_auth_manager.{h,cpp}      # PSN OAuth token 生命周期
│   ├── ps_remote_connector.{h,cpp}   # PSN session + 双 socket 打洞
│   ├── chiaki_adapter.{h,cpp}        # C callback、错误和日志适配
│   └── ps_input_mapper.{h,cpp}       # Switch HID -> ChiakiControllerState
├── stream/
│   └── ...                           # 继续共享，不引入平台协议
└── ui/
    ├── platform_activity.*           # Xbox / PlayStation 一级入口
    ├── ps_activity.*                 # PS 主机、注册、PSN 登录
    ├── stream_loading_activity.*     # 接收 IStreamRuntime
    └── stream_view.*                 # 接收 IStreamRuntime
```

不要求第一批提交一次创建全部文件；目录表示最终职责边界。

## 5. 最小共用接口

当前 `StreamView` 直接依赖 Xbox `StreamController`。为了让 PS 复用串流 UI，只抽取页面真正使用的方法：

```cpp
class IStreamRuntime {
public:
    virtual ~IStreamRuntime() = default;

    virtual StreamState state() const = 0;
    virtual std::string lastError() const = 0;
    virtual const stream::PerfStats& perfStats() const = 0;

    virtual int streamWidth() const = 0;
    virtual int streamHeight() const = 0;
    virtual stream::VideoBackend videoBackend() const = 0;

    virtual void updateInput() = 0;
    virtual void presentVideoFrame() = 0;
    virtual void setInputSuppressed(bool suppressed) = 0;
    virtual void stop() = 0;
};
```

实施方式：

- 现有 Xbox `StreamController` 实现该接口，原有公开方法保留；
- 新的 `PsStreamController` 实现同一接口；
- `StreamView` 和 loading activity 改为持有 `shared_ptr<IStreamRuntime>`；
- 不创建通用 `IProtocolSession`，不改造 Xbox WebRTC 内部状态机。

这样对 Xbox 的修改主要是接口适配和 UI 参数类型变化，协议行为不变。

## 6. PlayStation 主机模型与自动连接

### 6.1 明确区分主机身份

```cpp
struct PsConsole {
    std::optional<std::string> server_mac; // 12 位小写十六进制
    std::optional<std::string> psn_duid;   // 64 位小写十六进制
    std::string nickname;
    ChiakiTarget target;

    std::optional<RegisteredCredential> credentials;
    std::optional<PsLocalEndpoint> local;
    std::optional<PsRemoteEndpoint> remote;
};
```

其中：

- `server_mac` 是本地发现、PIN 配对和长期注册凭据的主键；
- `psn_duid` 是 PSN 设备目录和远程会话的主机标识；
- `credentials` 保存 RP-RegistKey、RP-Key/morning 和可选的主机登录 PIN；
- `local` 保存新鲜的内网 IP、发现时间和 READY/STANDBY 状态；
- `remote` 保存 PSN DUID、remote-play enabled 状态和 PSN 名称；
- 持久化精确的 `server MAC <-> PSN DUID` 关联；nickname 只用于首次、唯一匹配时提出关联，不能作为长期主键；
- DUID 在模型和 JSON 中始终保存为 64 位 hex，只在 hole-punch API 边界解码为 32 字节。

不得继续把 LAN host ID 和二进制 PSN DUID 塞入同一个 `stable_id`，也不得使用 DUID 前缀进行模糊合并。

### 6.2 页面加载与聚合流程

进入 PlayStation 页面后只加载本地 token、凭据和已保存主机，不自动发网络请求。页面提供两个独立按钮：

```text
Search LAN  -> 有界 Chiaki LAN discovery
Refresh PSN -> token refresh（需要时）+ PS5 device list
```

要求：

- 没有 PSN token 时仍可使用局域网发现和已配对主机；
- PSN 请求失败只显示远程不可用，不阻止页面打开；
- LAN 搜索必须主动发送 search packet，并最终进入 Complete/Failed，不能永久停在 Listening；
- LAN endpoint 带 last-seen，过期结果不能阻挡 PSN 路由；
- 同一主机只显示一个卡片；
- 卡片显示“局域网 Ready”“待机”“PSN 远程可用”“Remote Play 未开启”等真实状态；
- 登录成功返回页面时可以自动执行一次 PSN refresh，作为登录动作的明确延续。

### 6.3 自动路由策略

用户点击同一台主机时：

1. 如果存在新鲜的 READY 局域网结果，优先本地连接；
2. 新鲜的 STANDBY 结果进入 `Wake & Connect`：发送唤醒、等待同一 MAC 重新变为 READY，再使用最新 IP 连接；
3. 本地端点过期、不可达或建流前失败，且存在可用 PSN token/DUID，则转远程连接；
4. PSN-only 且 remote-play enabled 的 PS5 可直接远程连接，不要求预先保存传统 LAN PIN 配对凭据；
5. 远程路径先打通 control socket，再将 hole-punch session 交给 ChiakiSession；动态 PSN registration、data socket 打洞和媒体启动由 ChiakiSession 完成；
6. 媒体已经开始后断线，先按原路径重连一次，再决定是否切换路径，避免频繁创建 PSN session。

远程打洞不应在页面加载时预先启动，只在用户实际点击远程主机时创建。

## 7. PlayStation 会话流程

### 7.1 本地 Pair

```text
用户在 PS 上开启 Remote Play 并取得 8 位 Link/Add Device PIN
  -> LunarNX 输入 PIN
  -> chiaki regist
  -> 获得 server MAC / nickname / RP keys
  -> 以 server MAC 为主键写入 ps_credentials.json
  -> 若已有唯一匹配的 PSN device，确认并持久化 MAC <-> DUID 映射
```

该 8 位 PIN 是一次性配对输入，不能持久化为 console login PIN。注册凭据和 PSN token 必须与 Xbox token 分文件保存，采用原子写入，并确保不会进入 Git。

### 7.2 局域网连接

```text
fresh LAN discovery/manual IP
  -> Pair（仅未配对主机）
  -> local wakeup + wait for READY（STANDBY 时）
  -> Chiaki control/session handshake
  -> 协商分辨率、FPS、码率、codec
  -> Takion encrypted UDP
  -> FEC/reassembly
  -> callbacks
```

### 7.3 PSN 异地连接

```text
refresh PSN OAuth token（需要时）
  -> list PS5 devices
  -> create PSN Remote Play session
  -> start/wake console DUID
  -> create/exchange control offer + STUN/NAT information
  -> punch control socket
  -> hand hole-punch session to ChiakiSession
  -> control RUDP
  -> PSN registration (pin=0 + PSN account ID)
  -> obtain per-session RP-RegistKey + RP-Key
  -> session request + Ctrl + optional console login PIN
  -> Chiaki creates/exchanges data offer
  -> Chiaki punches data socket
  -> Senkusha / Takion / callbacks
```

PSN remote 并不是绕过 registration credential，而是把“事先通过 LAN PIN 配对取得 key”替换为“control RUDP 建立后动态 PSN registration 取得 key”。因此 PSN-only 且 Remote Play enabled 的 PS5 可以直接 Connect，UI 不应要求传统 Pair。

PSN 服务用于设备和会话协调以及辅助打洞。设计上不能假设一定存在类似 TURN 的通用媒体中继，因此严格 NAT/CGNAT 失败必须转化为清晰的用户错误。

### 7.4 会话状态机

```text
Idle
  -> ResolvingRoute
  -> Waking -> WaitingForConsole -> ConnectingLocal
     or RefreshingPsnToken -> CreatingRemoteSession
        -> StartingConsole -> PunchingControl
        -> RegisteringRemoteSession -> StartingControl
        -> WaitingForConsoleLoginPin（按需）
        -> PunchingData
  -> Negotiating
  -> WaitingForVideo
  -> Streaming
  -> Stopping -> Idle

任意阶段 -> Cancelling -> Cancelled
任意阶段 -> Error
```

loading 页必须保留到 Chiaki connected 且第一帧可用视频进入媒体管线。`chiaki_session_start()` 返回成功不等于已经可以进入 StreamView。

所有阻塞调用运行在 network/session worker；状态回调通过 `brls::sync()` 更新 UI。取消必须能中断 discovery、OAuth refresh、hole punch、session start、登录 PIN 等待和第一帧等待。

## 8. Chiaki 与共享媒体管线的边界

### 8.1 不复用的部分

PS 路径不经过：

- Xbox REST Session API；
- SDP/ICE；
- libpeer PeerConnection；
- DTLS-SRTP/SCTP DataChannel；
- Xbox RTP jitter buffer；
- XInput wire encoder。

Chiaki 负责：

- PS Remote Play 加密和会话控制；
- Takion/RUDP；
- 音视频解密和拆包；
- 视频 FEC、帧重组、参考帧恢复和 IDR 请求；
- Opus 音频帧排序；
- PS 控制器和反馈协议。

### 8.2 复用的部分

Chiaki 完成重组后，以下继续复用：

- `MediaPipeline` 的有界视频队列和工作线程；
- FFmpeg NVTEGRA 硬解；
- deko3d 零拷贝渲染、软件 fallback、后处理；
- Opus 解码和 audout；
- A/V sync；
- `PerfStats`、overlay 和串流页面。

### 8.3 `PsMediaBridge`

Chiaki 视频回调提供完整编码帧：

```cpp
bool onVideoSample(uint8_t* data,
                   size_t size,
                   int32_t frames_lost,
                   bool recovered);
```

桥接层负责：

- 立即复制回调缓冲区到 `MediaPipeline` 的有界队列；
- 使用协商 FPS 维护视频媒体时钟；
- `frames_lost` 推进时间线并更新统计；
- 将 Chiaki FEC/IDR 状态映射到共享恢复统计；
- 不把 PS 帧送入 Xbox RTP jitter buffer。

Chiaki 视频回调没有 Xbox RTP 时间戳。视频 PTS 应按协商帧率和丢失帧数生成，而不是直接用包到达时间：

```text
next_video_pts += (frames_lost + 1) * nominal_frame_duration
```

音频回调提供 Opus frame 和 `ChiakiAudioHeader`。桥接层使用采样率和 frame size 维护音频 sample clock，使音视频共享同一个 session epoch。

Chiaki 已经完成音频排序，因此共享管线需要增加一个“已排序 Opus”入口，或为 audio reorder 配置 bypass。Xbox 继续使用当前 reorder 策略。

### 8.4 编码支持

第一阶段固定：

- PS4：H.264；
- PS5：先强制 H.264；
- 音频：Opus 48 kHz stereo；
- HDR：关闭。

当前 `VideoDecoder`、关键帧检查和恢复逻辑都是 H.264 专用。HEVC 阶段再引入：

```cpp
enum class VideoCodec { H264, HEVC };
```

并将以下逻辑 codec 化：

- FFmpeg decoder/parser ID；
- VPS/SPS/PPS/IDR/CRA 检测；
- decoder reset gate；
- HDR 色彩空间和渲染参数。

不要为了第一版 PS 支持提前改动稳定的 Xbox H.264 路径。

## 9. 输入、震动和 DualSense

`GamepadReader` 可以共享，但编码器不能共享：

```text
Switch HID -> GamepadState
              ├-> XInputEncoder -> Xbox DataChannel
              └-> PsInputMapper -> ChiakiControllerState
```

映射规则按 Switch 按键物理位置映射到 PlayStation Cross/Circle/Square/Triangle，PS/Home、Options、Create/Share、触摸板按键使用独立组合键策略。

分阶段支持：

1. 基础按钮、摇杆、扳机；
2. 普通左右震动；
3. 触摸板模拟；
4. 陀螺仪/加速度计；
5. PS5 自适应扳机和音频触觉。

Xbox 现有 Guide chord 和 XInput 映射不改变。

## 10. UI 与用户流程

PlayStation 页面采用三段结构：

```text
PlayStation Remote Play
├── Account
│   ├── PSN 状态
│   └── Sign In / Refresh / Switch Account
├── My Consoles
│   ├── 已配对主机
│   ├── PSN PS5 设备
│   └── Refresh PSN
└── Local Network
    ├── 新鲜 LAN discovery 结果
    ├── Search LAN
    └── Pair PS4/PS5 by IP
```

卡片动作矩阵：

| 状态 | 主要动作 |
|---|---|
| LAN、未配对、READY | Pair |
| LAN、已配对、READY | Connect |
| LAN、已配对、STANDBY | Wake & Connect |
| PSN-only、Remote Play enabled | Connect |
| PSN-only、Remote Play disabled | 无连接按钮，显示开启方法 |
| 已保存主机但无可用路由 | Unavailable |

要求：

- Xbox 页面布局和行为尽量保持现状；
- PlayStation 页面按需创建 controller；
- PSN 登录不是进入 PlayStation 页面的前置条件；
- 页面打开只加载本地状态，Search LAN 与 Refresh PSN 都由按钮手动触发；
- 一张 PS 主机卡同时代表本地和远程连接能力；
- Pair 只表示传统 8 位 PIN 本地配对，不应显示在可直接 PSN remote 的主机上；
- loading 页展示具体阶段：唤醒、PSN 会话、control 打洞、动态 registration、登录 PIN、data 打洞、协商、等待视频；
- 串流页继续共用 overlay、性能面板和退出手势；
- 错误信息区分认证失败、Remote Play 未开启、配对失败、本地不可达、NAT 打洞失败、登录 PIN 错误和媒体启动失败。

详细页面与交互契约见 [PlayStation Page and Launch Flow Design](superpowers/specs/2026-07-31-playstation-page-and-launch-flow-design.md)。

## 11. 配置和持久化

建议在现有配置中新增平台命名空间，旧字段继续作为 Xbox 默认值，避免破坏已有用户配置：

```json
{
  "xbox": {
    "resolution": 1080,
    "video_backend": "hardware_zero_copy"
  },
  "playstation": {
    "resolution": 720,
    "fps": 60,
    "bitrate_kbps": 10000,
    "codec": "h264",
    "route": "auto",
    "video_backend": "hardware_zero_copy",
    "remote_port_guess_count": 0
  }
}
```

迁移策略：

- 读取时兼容当前顶层字段；
- 首次保存新设置时写入平台命名空间；
- 不强制一次性重写旧配置；
- token、注册 key 和主机映射不写入普通设置文件；
- 日志不得记录 OAuth token、RP key、account ID 或完整设备凭据。

## 12. 依赖与构建方案

### 12.1 版本统一

正式 Switch 链接只允许一份：

- curl 8.11.x；
- mbedTLS 3.4.x；
- zlib；
- Opus；
- FFmpeg。

不能同时链接 devkitPro curl 7.x 和 chiaki curl 8.x，也不能同时链接 mbedTLS 2.28 和 3.4，以免静态符号、结构 ABI 和 TLS 行为冲突。

### 12.2 curl 构建要求

共享 curl 至少启用：

- HTTP、HTTPS；
- WS、WSS；
- mbedTLS backend；
- IPv4；
- hostname verification。

可关闭当前不需要的 SSH、PSL、Brotli、MIME 和 Zstd 后端，以限制 NRO 体积和链接依赖。

### 12.3 TLS 校验与 Switch 平台限制

当前 Switch homebrew 环境通常没有像桌面系统一样可直接使用的完整 CA trust store。wiliwili 等 Switch 开源项目也常见关闭 curl 的 peer/hostname 校验；在没有证书链的情况下强行开启校验，会让本来可用的 Xbox 或 PSN HTTPS 请求全部失败。

因此这不是 PS 专有的硬阻塞，建议分成两个模式：

- **Compatibility mode（当前 MVP 默认）**：`CURL_VERIFY=0`，与现有 Xbox、wiliwili 的实际 Switch 生态保持一致；明确记录这是“加密但不验证服务器身份”，适合家庭网络和开发测试。
- **Verified mode（后续可选）**：提供 CA bundle、libnx SSL service 的系统证书桥接，或经过审慎维护的证书配置后开启 peer/hostname 校验。

关闭校验不是“完全没有安全风险”，但风险需要按使用场景评估：攻击者必须位于网络路径上并主动实施中间人攻击；在可信家庭网络中实际暴露面较低，而公共 Wi-Fi、恶意代理和不可信网络风险更高。PSN OAuth/Remote Play token 可能比普通目录请求更敏感，因此日志和 token 存储仍必须严格保护。

无论使用哪种模式，都必须：

- 继续使用 HTTPS/WSS 加密传输；
- 不把 token 放入日志、错误文本或诊断 trace；
- 在设置或状态页显示当前 TLS 模式；
- 不把关闭校验误报为“已完成服务器身份验证”；
- 对 Xbox、PSN OAuth、PSN WebSocket/API 分别记录兼容性结果。

Verified mode 可以作为后续增强项，不阻塞首个可用的 PS 局域网/异地串流 MVP。

### 12.4 chiaki mbedTLS 3 适配

探针补丁仍访问 `mbedtls_ecdh_context` 私有成员，只适合作为可行性证明。正式方案应优先：

1. 使用 mbedTLS 3 公共 ECDH API 重写 chiaki 兼容层；
2. 将补丁限制在 `tools/chiaki_switch/` 并记录上游 commit；
3. 增加 ECDH、GMAC/GCM fixture 测试；
4. 将可上游的修复提交回 chiaki-ng。

### 12.5 可复现构建

新增 Docker 内依赖构建脚本，产出：

```text
lib/switch/libcurl.a
lib/switch/libchiaki.a
lib/switch/include/curl/
lib/switch/include/chiaki/
```

脚本应固定：

- devkitA64 image；
- chiaki-ng commit；
- curl 和 mbedTLS 版本；
- CMake cache 参数；
- protobuf/nanopb 生成工具版本；
- 所有 Switch patch 的应用顺序和校验。

生成库不应依赖实验 worktree 路径。

## 13. AGPL-3.0 合规方案

用户已经接受 AGPL-3.0。直接链接 chiaki-lib 后，LunarNX 发布按 AGPL-3.0 要求执行：

- 将项目许可证和分发说明更新为 AGPL-3.0 兼容形式；
- 保留 chiaki-ng、curl、mbedTLS、FFmpeg 等第三方版权和许可证；
- 发布 NRO 时同步提供对应版本的完整可构建源代码；
- 包含 LunarNX 对 chiaki-ng 的所有 Switch/mbedTLS 修改；
- 提供构建脚本、依赖版本和补丁，不只发布预编译静态库；
- 不提交或发布用户 token、注册凭据、日志和模拟器数据。

正式改许可证前应确认项目所有既有源码和资源有权以 AGPL-3.0 分发。

## 14. 测试策略

### 14.1 单元和 fixture 测试

新增：

- MAC、DUID 规范化和严格解码测试；
- registration credential v2 持久化、旧格式迁移和原子写测试；
- pairing PIN 不落盘、console login PIN 可选保存测试；
- 本地发现、凭据和 PSN device list 的精确合并测试；
- MAC/DUID 持久关联和重复 nickname 歧义测试；
- local-first/remote-fallback 路由与 LAN endpoint freshness 测试；
- LAN 搜索 generation、完成、零结果和取消测试；
- Wake & Connect 等待同一 MAC 变为 READY 的测试；
- remote control-hole -> session-owned registration/data-hole 顺序测试；
- hole-punch cancel 和单一 ownership/fini 测试；
- console login PIN 请求、错误重试和取消测试；
- Chiaki video callback 到有界队列测试；
- video/audio synthetic clock 和丢帧推进测试；
- PS controller mapping 测试；
- PS session cancel/stop 幂等测试；
- token 和注册凭据不出现在日志的测试；
- mbedTLS 3 ECDH/GCM compatibility fixtures。

### 14.2 Xbox 回归

每个依赖、共享接口或媒体修改都运行：

```sh
python3 tests/xbox_stream_session_order_test.py
python3 tests/libpeer_sctp_config_test.py
python3 tests/libpeer_dtls_read_loop_test.py
python3 tests/datachannel_ppid_test.py
python3 tests/switch_nro_bss_test.py
```

并至少验证：

- Xbox 主机列表；
- xCloud 游戏列表；
- WebRTC 会话建立；
- 实际画面和音频；
- 输入和退出；
- keepalive 和停止清理。

### 14.3 PlayStation 验证矩阵

| 场景 | PS4 | PS5 |
|---|---:|---:|
| 局域网发现 | 必测 | 必测 |
| PIN 注册 | 必测 | 必测 |
| 本地唤醒 | 必测 | 必测 |
| H.264 720p30/60 | 必测 | 必测 |
| H.264 1080p60 | PS4 Pro | 必测 |
| Opus 音频 | 必测 | 必测 |
| 基础输入/震动 | 必测 | 必测 |
| PSN 设备列表 | 必测 | 必测 |
| 异地打洞 | 必测 | 必测 |
| HEVC/HEVC HDR | 不适用 | 后续阶段 |

网络场景至少包含：

- 同一 Wi-Fi；
- 有线 PS + Wi-Fi Switch；
- 不同运营商；
- 一侧 CGNAT；
- 严格 NAT 失败的错误提示；
- 网络切换、丢包和取消。

### 14.4 Switch 验证

每次 Switch 代码或构建变化：

- Docker 全量构建；
- `switch_nro_bss_test.py`；
- `git diff --check`；
- Ryubing 基础 UI/生命周期回归；
- 真实 Switch 上的本地和异地实流测试。

## 15. 分阶段实施计划

### Phase 0：依赖和许可证基础

- 将 curl 8.11 + mbedTLS 3.4 构建正式化；
- 将 chiaki-lib 构建脚本和补丁迁入主分支；
- 完成 mbedTLS 3 公共 API 适配；
- 开启 TLS 证书校验；
- 完成 AGPL 许可证和 third-party notices；
- 做一次真实 Switch Xbox 回归。

退出条件：统一依赖可在全新 checkout 中一条 Docker 命令重建，Xbox 真机功能不回退。

### Phase 1：共用运行时边界

- 增加 `IStreamRuntime`；
- 让现有 Xbox controller 适配该接口；
- loading/stream view 改用接口；
- 不增加 PS 功能，先做 Xbox 回归。

退出条件：代码结构改变但 Xbox UI、会话、媒体、输入行为完全一致。

### Phase 2：PS 主机和注册

- PlayStation 页面；
- 局域网 discovery；
- PS4/PS5 PIN 注册；
- 凭据存储；
- 本地唤醒；
- 主机卡片状态。

退出条件：真实 Switch 能发现、注册并唤醒一台 PS4/PS5，不启动媒体流。

### Phase 3：PS 局域网 H.264 串流

- `PsStreamSession`；
- `PsMediaBridge`；
- H.264 + Opus 接入共享媒体管线；
- 基础手柄输入和普通震动；
- stop/cancel/error 生命周期。

退出条件：真实 Switch 局域网 720p60 连续运行，画面、音频、输入、退出正常；Xbox 回归仍通过。

### Phase 4：PSN 异地串流和自动选择

- PSN OAuth/token refresh；
- PSN device list；
- DUID/MAC 主机关联；
- control/data 双 socket 打洞；
- local-first/remote-fallback；
- NAT 和 token 错误提示。

退出条件：同一主机卡片在家中自动本地直连，在外网自动走 PSN，用户无需选择模式。

### Phase 5：PS5 HEVC 和高级输入

- codec-aware `VideoDecoder`；
- HEVC NVTEGRA、VPS/SPS/PPS/IDR/CRA 恢复；
- HDR 色彩和 tone mapping 决策；
- 触摸板、运动传感器、自适应扳机和音频触觉。

退出条件：PS5 HEVC 稳定且 H.264 Xbox/PS 路径无回归。

### Phase 6：稳定性和发布

- 自动重连和路由重试；
- 长时间运行、Suspend/Resume；
- token/凭据迁移和清理；
- 网络质量统计；
- 完整 AGPL source release 验证。

## 16. 主要风险和控制措施

| 风险 | 影响 | 控制措施 |
|---|---|---|
| 修改共享媒体导致 Xbox 回归 | 高 | H.264 第一版少改媒体；每阶段真实 Xbox A/B |
| 两套 curl/mbedTLS 静态冲突 | 高 | 全项目统一 curl 8.11/mbedTLS 3.4 |
| mbedTLS 私有结构补丁脆弱 | 高 | 改用公共 API并加 crypto fixtures |
| 关闭 TLS 校验泄露 PSN token | 高 | CA bundle、hostname verify 作为发布门槛 |
| 严格 NAT/CGNAT 打洞失败 | 中高 | 清晰诊断、端口猜测配置、保留手动地址能力 |
| callback 时间戳不正确导致 A/V 漂移 | 中高 | 基于 FPS/sample clock 的统一 epoch 和长时测试 |
| HEVC 改动 H.264 稳定路径 | 中高 | HEVC 延后、codec 分支和双平台回归 |
| NRO 体积/BSS 超限 | 中 | 每次全量构建运行 BSS guard，避免静态大缓冲 |
| AGPL 分发不完整 | 高 | source bundle、patch、构建脚本作为发布产物 |
| PSN API/协议变化 | 中 | chiaki commit 固定、错误可观测、升级隔离在适配层 |

## 17. 完成定义

双平台目标完成需同时满足：

- Xbox Remote Play 和 xCloud 保持当前能力；
- PS4/PS5 能注册、发现、唤醒并局域网串流；
- PSN 登录后能列出远程主机并异地串流；
- 同一 PS 主机自动选择本地或远程路径；
- H.264/Opus 共用现有硬解、渲染和音频后端；
- PS5 HEVC 按独立阶段交付，不阻塞 H.264 MVP；
- 统一依赖可复现构建，TLS 校验开启；
- NRO BSS 低于 32 MiB；
- Xbox、PS 均经过真实 Switch 实流验证；
- 发布物满足 AGPL-3.0 和第三方许可证要求。
