# LunarNX

[English](README.md) | 简体中文

面向 Nintendo Switch 自制软件环境的非官方 Xbox Remote Play 与 Xbox Cloud Gaming 客户端。

LunarNX 实现了 Xbox 串流客户端所需的完整链路：Microsoft 账号认证、Xbox 会话 API、WebRTC 传输、H.264/Opus 解码、Switch 画面与音频输出、手柄输入和震动反馈。

> [!WARNING]
> LunarNX 仍处于早期开发阶段，主要用于真实 Nintendo Switch 硬件上的开发与测试。它需要能够运行 NRO 的 Switch 自制软件环境，无法在未经修改的零售版主机上直接运行；不同网络、账号、游戏、系统版本以及 Xbox 服务端变化都可能影响兼容性。

## 项目状态

真实 Switch 硬件是最终兼容性目标。Ryubing/Ryujinx 可用于开发回归，但模拟器能够运行不代表实机一定兼容。

| 功能 | 当前状态 |
| --- | --- |
| 同一局域网内连接自己的 Xbox 主机 | 当前主要实机测试路径 |
| 通过互联网连接自己的 Xbox 主机 | 实验性；依赖 UDP/NAT 直连能力 |
| Xbox Cloud Gaming（xCloud） | 实验性 |
| 720p / 1080p / 1080p HQ | 已提供；稳定性和实际码率取决于网络及服务端 |
| H.264 硬件解码 | 已通过 Switch NVDEC 实现 |
| 原生 IPv6 | 可选编译功能，默认 Switch 构建关闭 |
| TURN 中继 | 尚未实现 |

异地串流无法保证在所有网络环境中工作。LunarNX 可以使用公网 IPv4、编译启用后的原生 IPv6，以及从 Xbox Teredo ICE candidate 派生出的 IPv4 端点；但当前使用的 legacy WebRTC 栈尚不具备完整的 TURN relay 回退能力。

## 主要功能

- Microsoft Device Code 登录，Token 仅保存在本地
- 查找并唤醒账号中注册的 Xbox 主机
- 浏览和搜索 xCloud 游戏库，包含最近游玩与新加入内容
- Xbox GSSV 会话、SDP/ICE 交换、旧会话清理、Keepalive 和断线重连
- 使用 Tegra X1 NVDEC 进行 H.264 硬件解码
- deko3d 零拷贝渲染，并提供硬件 Copy-out 与软件解码回退路径
- 可选 EASU 放大、RCAS 锐化和抖动处理
- Opus 音频解码与低延迟 libnx Audren 输出
- Switch 手柄到 Xbox 输入映射，以及四马达震动支持
- 720p 10 Mbps、1080p 20 Mbps、1080p HQ 30 Mbps 接收配置
- 串流性能面板，可查看码率、丢包、解码、渲染、音频和 WebRTC 状态
- 英文、简体中文与繁体中文界面

## 使用要求

- 能够运行 Homebrew NRO 的 Nintendo Switch，通常需要 Atmosphère CFW
- 强烈建议使用 Title Override / 完整内存模式；Applet Mode 的可用内存可能不足以支持串流和硬件解码
- 已启用 Remote Play 的 Xbox One 或 Xbox Series 主机，或者具备 Xbox Cloud Gaming 权限的账号
- 稳定的 5 GHz Wi-Fi 或有线网络
- 使用模拟器时，请自行准备合法获得的固件和系统文件

xCloud 的可用性取决于账号权限和地区。通常需要 Game Pass Ultimate，部分符合条件的免费游戏可能有所不同。

## 安装

1. 从 [Releases 页面](https://github.com/thinkzhou/LunarNX/releases) 下载 `LunarNX.nro`。
2. 将文件复制到 SD 卡：

   ```text
   sdmc:/switch/LunarNX/LunarNX.nro
   ```

3. 以 Title Override / 完整内存模式打开 Homebrew Menu，然后启动 LunarNX。

请勿公开或分享 `sdmc:/switch/LunarNX/` 目录中的内容。该目录可能包含 Microsoft/Xbox 登录凭据、账号缓存和诊断日志。

## 首次使用

1. 选择 **开始登录**。
2. 在手机或电脑上打开界面显示的 Microsoft 设备登录地址并输入验证码。
3. 登录完成后，选择账号中的 Xbox 主机或进入 xCloud 游戏库。
4. 在设置中选择 720p、1080p 或 1080p HQ。
5. 选择 **开始游戏** 或 **唤醒并连接**。

首次建立会话可能需要一分钟。主机处于休眠状态时，可能需要多次唤醒尝试。

## 手柄操作

LunarNX 按按键的物理位置映射 Xbox 布局，因此 Nintendo 与 Xbox 的 ABXY 字母会进行转换。

| Nintendo Switch | Xbox 操作 |
| --- | --- |
| B（下） | A |
| A（右） | B |
| Y（左） | X |
| X（上） | Y |
| L / R | LB / RB |
| ZL / ZR | LT / RT |
| Minus | View |
| Plus | Menu |
| L + R + Plus（同时按下） | Xbox Guide / Nexus 键 |
| 左摇杆按下 | L3 |
| 右摇杆按下 | R3，同时切换性能详情面板 |
| 十字键 / 摇杆 | 十字键 / 摇杆 |

Switch 的 ZL/ZR 是数字按键，因此 Xbox 扳机输入只有 0% 或 100%。

串流过程中，同时按下 **L + R + Plus** 即可打开 Xbox 导航菜单。LunarNX 会将这个组合键作为单独的 Xbox Guide / Nexus 输入发送，而不会同时发送 LB、RB 和 Menu。

停止串流时，请在三秒内连续两次同时按下 **Minus + Plus**。单独按下 Minus 或 Plus 仍会作为 Xbox View 或 Menu 输入发送。

## 从源码构建

### 环境要求

- Git
- Docker
- 固定镜像 `devkitpro/devkita64:20251117`
- macOS 或 Linux 主机

不要在 macOS 上直接构建 Switch 目标库。Switch 依赖库与 NRO 必须在 Docker 的 devkitA64 环境中构建。

### 准备依赖

```sh
git clone https://github.com/thinkzhou/LunarNX.git
cd LunarNX
./scripts/setup_dependencies.sh
```

该脚本会获取固定版本的 Borealis 与 legacy libpeer，并应用 LunarNX 跟踪维护的 libpeer 补丁。这些本地依赖 checkout 会被主仓库忽略。

### 构建 Switch NRO

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    set -e
    export DEVKITPRO=/opt/devkitpro
    export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
    make -f Makefile.switch clean
    make -f Makefile.switch -j$(nproc) \
      NETWORK_DIAG=0 CURL_PROVIDER=wiliwili CURL_VERIFY=0 \
      CURL_VERBOSE=0 CURL_TIMEOUT_MS=30000
  '
```

构建产物位于：

```text
build/switch/LunarNX.nro
```

默认 Switch 构建使用 `IPV6=0`。如需编译原生 IPv6 支持，可在 make 命令中传入 `IPV6=1`。这不会增加 TURN relay 支持，也不代表在 IPv6-only 或严格 NAT 网络中一定能够连接。

### 桌面开发构建

桌面目标用于开发、协议探针和回归测试，不能替代 Switch 实机验证。

```sh
make -f Makefile.desktop -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
make -f Makefile.desktop stream_tests
make -f Makefile.desktop xcloud_session_support_test
```

常用诊断目标：

```sh
make -f Makefile.desktop auth_test
make -f Makefile.desktop sdp_probe
make -f Makefile.desktop xcloud_handshake_probe
```

### 验证

修改 Switch 或串流代码后，请运行对应的 focused tests，并至少执行：

```sh
scripts/check_stream_regressions.sh
python3 tests/xbox_stream_session_order_test.py
python3 tests/libpeer_sctp_config_test.py
python3 tests/libpeer_dtls_read_loop_test.py
python3 tests/datachannel_ppid_test.py
python3 tests/switch_nro_bss_test.py
git diff --check
```

Switch NRO 的 BSS 回归限制为 32 MiB。真实 Switch 仍是最终验证目标。模拟器与本地 mock 串流流程请参考 [docs/ryujinx_testing.md](docs/ryujinx_testing.md)。

如修改了 GLSL shader，请在 devkitPro 环境中重新生成 `.dksh` 文件：

```sh
./scripts/compile_shaders.sh
```

## 架构概览

```text
Microsoft Device Code 认证
             │
             ▼
       Xbox REST / GSSV API
  主机发现、云游戏目录、会话管理
             │
             ▼
      legacy libpeer WebRTC
 SDP + ICE + DTLS-SRTP + SCTP
       │                    │
       ▼                    ▼
H.264 / Opus 媒体       Xbox 输入与控制
       │                    │
       ▼                    ▼
NVDEC + deko3d        Joy-Con / Pro 手柄
Audren 音频           XInput + 震动
```

主要代码目录：

| 路径 | 职责 |
| --- | --- |
| `src/auth/` | Microsoft/Xbox 认证与 Token 存储 |
| `src/api/` | HTTP、主机发现、云游戏目录与 Xbox 会话 API |
| `src/app/` | 串流配置、会话生命周期、SDP/ICE、数据通道和控制器编排 |
| `src/webrtc/` | LunarNX 对当前 legacy libpeer 的封装 |
| `src/stream/` | H.264/Opus 解码、渲染、音频、同步和性能统计 |
| `src/input/` | Switch 手柄读取、Xbox 输入包编码和震动 |
| `src/ui/` | Borealis 页面、设置、列表、串流界面和性能面板 |
| `tools/mock_xbox/` | 用于模拟器测试的本地 Xbox WebRTC mock 服务 |

## 隐私与安全

- 登录 Token 仅保存在 SD 卡本地。
- 请勿上传 Token 文件、完整诊断日志、原始 Xbox API 响应、主机标识符或公网 IP。
- Release 构建默认关闭应用日志、网络诊断和原始 Xbox 响应跟踪。
- Debug 构建可能记录敏感的账号与网络元数据。

报告安全问题或分享日志前，请阅读 [SECURITY.md](SECURITY.md)。

## 参与贡献

欢迎提交代码和实机测试报告。发起 Pull Request 前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。

有效的测试报告应包含画质配置、Switch 型号和系统版本、网络拓扑、局域网或异地测试类型，以及最小化且已脱敏的日志片段。请勿附上完整日志。

## 致谢

LunarNX 使用或参考了多个优秀的开源项目：

- [libpeer](https://github.com/sepfy/libpeer)
- [Borealis](https://github.com/XITRIX/borealis)
- [FFmpeg](https://github.com/FFmpeg/FFmpeg)、[wiliwili](https://github.com/xfangfang/wiliwili) 与 Switch NVDEC 相关工作
- [libnx](https://github.com/switchbrew/libnx) 与 [deko3d](https://github.com/devkitPro/deko3d)
- [XStreaming](https://github.com/Geocld/XStreaming)
- [Greenlight](https://github.com/unknownskl/greenlight)
- [Moonlight-Switch](https://github.com/XITRIX/Moonlight-Switch)
- [libnxbox](https://github.com/ursusworks/libnxbox)
- [xbox-xcloud-player](https://github.com/unknownskl/xbox-xcloud-player)

依赖许可证和二进制分发义务请查看 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 许可证

LunarNX 自有源码使用 [MIT License](LICENSE)。第三方源码、库、补丁和最终链接产物仍受各自许可证约束。

当前 Switch FFmpeg 构建启用了 GPL 组件。任何分发链接后 NRO 的人员，都必须检查并履行该次依赖构建对应的许可证义务。`THIRD_PARTY_NOTICES.md` 仅作为工程清单，不构成法律建议。

LunarNX 与 Microsoft、Xbox、Nintendo 以及文中提到的开源项目维护者不存在隶属、授权或背书关系。所有产品名称和商标均归其各自所有者所有。
