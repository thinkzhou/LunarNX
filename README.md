# LunarNX

> Xbox Remote Play / xCloud 串流客户端 for Nintendo Switch (Atmosphère CFW)

LunarNX 让你在破解的 Nintendo Switch 上游玩你自己的 Xbox 主机或 Xbox Cloud Gaming (xCloud) 的游戏。它实现了完整的 Xbox GameStream over WebRTC 协议栈：从 MSAL OAuth2 认证 → Xbox REST API → WebRTC SDP/ICE 信令 → NVDEC 硬件解码 → deko3d GPU 渲染 → audout 音频输出 → Switch HID 手柄输入映射，提供低延迟、零拷贝的端到端串流体验。

> [!WARNING]
> LunarNX 仍处于早期开发阶段，需要 Atmosphère CFW，不适用于未破解的 Switch。
> 局域网主机串流是当前的主要实机验收路径；异地串流依赖两端 UDP/NAT
> 连通性，而 legacy libpeer 尚未实现完整 TURN relay。xCloud 也属于实验性功能。

---

## 功能

- **认证**：MSAL Device Code Flow，手机上访问 microsoft.com/link 输入代码即可登录
- **主机串流**：连接你局域网内的 Xbox 主机，串流已安装的游戏
- **视频**：H.264 硬件解码 (Tegra X1 NVDEC)，deko3d GPU 零拷贝 NV12→RGB 渲染，BT.709 色域转换
- **音频**：Opus 软解码 (48kHz 立体声)，libnx audout 低延迟输出
- **手柄输入**：Switch → Xbox 按键物理位置映射，XInput 线格式编码，HD 震动 (4 马达)
- **性能叠加**：实时 FPS / 解码延迟 / 丢包统计，R3 按键切换显示
- **稳定性**：Session keepalive 保活，断连自动重连 (指数退避)，Token 自动刷新
- **音画同步**：基于时间戳的帧调度，>5ms 延迟丢帧，>2ms 提前 sleep

## 截屏

欢迎提交不含账号、主机标识符或公网 IP 的实机截图。

## 安装

### 前提

- 已破解的 Nintendo Switch (Atmosphère CFW)，能够启动 Title Mode（需要完整 ~3.2GB RAM）
- Xbox One / Xbox Series 主机；xCloud 通常需要 Game Pass Ultimate，部分免费游戏可能例外
- 5GHz WiFi 网络

### 下载

从 [Releases](https://github.com/thinkzhou/LunarNX/releases) 下载 `LunarNX.nro`，放到 SD 卡：

```
sdmc:/switch/LunarNX/LunarNX.nro
```

### 首次使用

1. 通过 hbmenu 启动 LunarNX（建议用 [NSP Forwarder](https://nsp-forwarder.vercel.app/moonlight) 创建桌面快捷方式以获得 Title Mode 完整内存）
2. 界面上会显示一个 8 位代码和 microsoft.com/link 网址
3. 用手机或电脑浏览器访问该网址，输入代码，用你的 Microsoft 账号登录
4. 认证成功后，LunarNX 会显示你的 Xbox 主机列表
5. 选择分辨率 (720p/1080p)，点击 Connect 开始串流
6. 串流中按 `-` (Minus) 退出，按 `R3` (右摇杆按下) 切换性能统计显示

### 手柄按键映射

| Switch | Xbox |
|--------|-----|
| A (右侧) | B |
| B (下方) | A |
| X (上方) | Y |
| Y (左侧) | X |
| ZL | LT (数字: 按下=100%, 松开=0%) |
| ZR | RT (数字: 按下=100%, 松开=0%) |
| L | LB |
| R | RB |
| - | View |
| + | Menu |
| L + R + Plus | Xbox Guide / Nexus |
| 左摇杆按下 | L3 |
| 右摇杆按下 | R3 |
| 十字键 | 十字键 |

> 注：Switch 的 ZL/ZR 是数字按键（无模拟行程），所以 LT/RT 输入只有 0% 或 100% 两种状态。

## 桌面测试

在烧录到 Switch 实机之前，可以在 macOS 上验证认证和 API 流程：

```bash
# 安装依赖
brew install openssl curl

# 编译并运行
make -f Makefile.desktop auth_test
./build/pc/lunar_auth_test
```

测试程序会：
1. 显示设备码和 microsoft.com/link 网址
2. 等你在浏览器完成登录后按 Enter
3. 获取 token 并显示你的 Gamertag
4. 列出你的 Xbox 主机
5. 创建一个测试会话并立即删除

## 从源码构建

### 环境

- macOS (Apple Silicon / Intel) 或 Linux
- Docker
- 固定镜像 `devkitpro/devkita64:20251117`
- Git（用于获取忽略的 Borealis 与 legacy libpeer 源码）

Switch 目标不支持在 macOS 上直接编译。先准备固定版本的本地依赖：

```sh
./scripts/setup_dependencies.sh
```

该脚本会获取 Borealis，并在指定 libpeer commit 上应用
`tools/libpeer_legacy/legacy-libpeer-switch.patch`。这两个 checkout 都被 Git 忽略。

### 构建

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    set -e
    export DEVKITPRO=/opt/devkitpro
    export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
    make -f Makefile.switch clean
    make -f Makefile.switch -j$(nproc) \
      IPV6=0 APP_DIAG=0 NETWORK_DIAG=0 XBOX_RESPONSE_TRACE=0 \
      CURL_PROVIDER=wiliwili CURL_VERIFY=0 CURL_VERBOSE=0 \
      CURL_TIMEOUT_MS=30000
  '
```

输出为 `build/switch/LunarNX.nro`。构建后建议运行：

```sh
python3 tests/switch_nro_bss_test.py
```

桌面版只用于开发测试，不代表 Switch 实机兼容性：

```sh
cmake -B build/pc -DPLATFORM_DESKTOP=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/pc -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

### shader 编译

如果修改了 `shaders/` 目录下的 GLSL 文件，需要用 `uam` (deko3d shader compiler) 重新编译：

```bash
uam -s glsl -o romfs/shaders/texture_fsh.dksh shaders/texture_fsh.glsl
uam -s glsl -o romfs/shaders/basic_vsh.dksh shaders/basic_vsh.glsl
```

## 架构

```
┌────────────────────────────────────────────────┐
│                  Nintendo Switch                 │
│  ┌──────┐ ┌──────┐ ┌────────┐ ┌─────────────┐ │
│  │ Auth │ │ API  │ │ WebRTC │ │   Input      │ │
│  │MSAL  │ │REST  │ │libpeer │ │   HID→XInput │ │
│  │ECDSA │ │JSON  │ │SDP/ICE │ │   HD Rumble  │ │
│  └──┬───┘ └──┬───┘ └───┬────┘ └──────┬──────┘ │
│     │        │         │              │         │
│     │        │    ┌────┴──────────────┘         │
│     │        │    │  Stream Pipeline             │
│     │        │    │  ┌───────────────────────┐  │
│     │        │    │  │ FFmpeg NVDEC H.264    │  │
│     │        │    │  │ FFmpeg Opus (software) │  │
│     │        │    │  └───────────┬───────────┘  │
│     │        │    │              │              │
│  ┌──┴────────┴────┴──────────────┴──────────┐  │
│  │           Render & Audio Output           │  │
│  │  deko3d NV12→RGB BT.709   │  audout PCM  │  │
│  └───────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────┐  │
│  │          borealis UI Framework             │  │
│  │   Auth → Console List → Stream + Overlay   │  │
│  └───────────────────────────────────────────┘  │
└────────────────────────────────────────────────┘
          │ WiFi 5GHz, 802.11ac
          ▼
┌──────────────────────────────────┐
│        Xbox / Azure Cloud         │
│  GameStream REST API (WebRTC)     │
│  H.264 + Opus / SCTP DataChannel  │
└──────────────────────────────────┘
```

## 依赖库

本项目站在以下优秀开源项目的肩膀上：

| 项目 | 用途 | 许可证 |
|------|------|--------|
| [libpeer](https://github.com/sepfy/libpeer) | 轻量级 WebRTC C 实现 (SDP/ICE/DTLS-SRTP/SCTP) | MIT |
| [averne/FFmpeg](https://github.com/averne/FFmpeg) | H.264 NVDEC 硬件解码 + Opus 软件解码 | LGPL/GPL |
| [borealis](https://github.com/XITRIX/borealis) | Switch 风格 C++ UI 框架 | MIT |
| [deko3d](https://github.com/devkitPro/deko3d) | Switch 原生底层 GPU API (NV12→RGB shader) | Zlib |
| [libnx](https://github.com/switchbrew/libnx) | Switch 硬件抽象层 (HID, audout, fs) | ISC |
| [mbedtls](https://github.com/Mbed-TLS/mbedtls) | DTLS/SRTP 加密 (ECDSA P-256 签名) | Apache 2.0 |
| [libsrtp](https://github.com/cisco/libsrtp) | SRTP 媒体加密 | BSD-3 |
| [cJSON](https://github.com/DaveGamble/cJSON) | 轻量 JSON 解析 | MIT |
| [devkitPro](https://devkitpro.org) | Switch 交叉编译工具链 | GPL |

## 参考项目

LunarNX 在协议逆向、架构设计和技术验证上参考了以下开源项目：

| 项目 | 说明 |
|------|------|
| [XStreaming](https://github.com/Geocld/XStreaming) | iOS/Android Xbox 串流客户端 — Xbox 协议、XInput 线格式、Rumble 协议的主要参考 |
| [libnxbox](https://github.com/ursusworks/libnxbox) | Switch Xbox 串流客户端 — 验证了 libpeer + NVDEC + deko3d 技术路线在 Switch 上可行 |
| [Moonlight-Switch](https://github.com/XITRIX/Moonlight-Switch) | Switch NVIDIA GameStream 客户端 — deko3d 渲染、audout 音频、borealis UI 等 Switch 平台技术模式参考 |
| [xbox-xcloud-player](https://github.com/unknownskl/xbox-xcloud-player) | JS xCloud WebRTC 库 — Xbox SDP 配置格式参考 |
| [Greenlight](https://github.com/unknownskl/greenlight) | Xbox 串流桌面客户端 — MSAL Device Code 认证流程参考 |

## 许可证

项目自有源码使用 [MIT License](LICENSE)。第三方源码、预编译库和最终链接产物
同时受各自许可证约束，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。特别是当
FFmpeg 以 `--enable-gpl --enable-version3` 构建时，分发链接后的 NRO 需要同时履行 GPLv3
及其他第三方许可证要求。

---

*LunarNX 不隶属于 Microsoft、Xbox 或 Nintendo。所有商标均为其各自所有者的财产。*
