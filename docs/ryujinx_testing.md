# Ryujinx NRO 模拟器测试指南

## 概述

在 macOS 上使用 Ryujinx/Ryubing 模拟器测试 Switch NRO 文件，无需真机。

当前建议分两类使用：

- **网络/API 流程测试**：优先使用 Ryubing Canary 1.3.333，主程序自带 `--no-gui` headless 模式。
- **快速加载 smoke test**：旧的 Axenov/Museum Ryujinx Headless SDL2 仍可用，但公网网络行为不可靠。

模拟器只能作为开发期回归工具；真实 Switch 仍然是最终判断标准。

## 文件位置

| 文件 | 路径 |
|------|------|
| Ryubing Canary 安装包 | `~/Downloads/ryujinx-canary-1.3.333-macos_universal.app.tar.gz` |
| Ryubing Canary 可执行文件 | `/tmp/ryubing-canary-1.3.333/Ryujinx.app/Contents/MacOS/Ryujinx` |
| 旧 Headless 二进制 | `~/work/self/ryujinx/publish_headless/Ryujinx.Headless.SDL2` (38MB, arm64) |
| 旧 Headless 源码 | `~/work/self/ryujinx/` (来自 git.axenov.dev/Museum/ryujinx) |
| Ryujinx 数据目录 | `~/work/self/ryujinx-data/` |
| prod.keys | `~/work/self/ryujinx-data/system/prod.keys` (用户自备) |
| 固件 NCA | `~/work/self/ryujinx-data/bis/system/Contents/registered/*.nca/00` (235个) |
| 固件 ZIP | `~/Downloads/Firmware.20.5.0.zip` (用户自备，可选) |
| Smoke test NRO | `build/switch-smoke/LunarNXSmoke.nro` (204KB) |
| 完整 LunarNX NRO | `build/switch/LunarNX.nro` |
| 测试脚本 | `scripts/run_nro_test.sh` |
| Ryubing 启动脚本 | `scripts/run_ryubing_nro.sh` |
| 按键发送工具 | `/tmp/SendKey` (Swift, 发送 CGEvent) |
| 按键发送工具2 | `/tmp/SendKeyToApp` (Swift, 定向进程) |

## 环境变量

```bash
# .NET SDK (Ryujinx 编译需要)
export PATH="$HOME/dotnet:$PATH"   # .NET 8.0 + 9.0

# 代理 (可选)
export https_proxy=http://127.0.0.1:7897
export http_proxy=http://127.0.0.1:7897
```

## 测试命令

```bash
# 推荐：Ryubing Canary headless 模式运行完整 LunarNX
./scripts/run_ryubing_nro.sh build/switch/LunarNX.nro

# 推荐：Ryubing Canary headless 模式运行网络探针
./scripts/run_ryubing_nro.sh build/switch-netprobe/LunarNXNetProbe.nro

# 本地 Xbox mock 串流，并在首个合法手柄包后发送一次测试震动
python3 tools/mock_xbox/mock_xbox_server.py \
  --video /tmp/lunarnx_test_720p60.mp4 \
  --public-ip 192.168.9.226 \
  --http-port 8080 \
  --send-test-rumble

# 如需保留少量 Ryubing 自身日志，可使用过滤模式
RYUBING_LOG_MODE=filtered ./scripts/run_ryubing_nro.sh build/switch/LunarNX.nro

# 旧 Headless：Smoke test (按键检测版, 15s 超时)
./scripts/run_nro_test.sh build/switch-smoke/LunarNXSmoke.nro 15

# 旧 Headless：完整 LunarNX
./scripts/run_nro_test.sh build/switch/LunarNX.nro 30

# 旧 Headless：网络测试 NRO。Ryujinx Headless 默认禁用 guest 网络，必须显式打开
RYUJINX_ENABLE_INTERNET=1 ./scripts/run_nro_test.sh build/switch-netprobe/LunarNXNetProbe.nro 60

# 旧 Headless：手动运行
~/work/self/ryujinx/publish_headless/Ryujinx.Headless.SDL2 \
  --root-data-dir ~/work/self/ryujinx-data \
  --disable-file-logging \
  --enable-debug-logs \
  --disable-docked-mode \
  --ignore-missing-services \
  --use-hypervisor false \
  --enable-internet-connection \
  build/switch-smoke/LunarNXSmoke.nro
```

## 关键 CLI 参数

| 参数 | 说明 |
|------|------|
| `--no-gui` | Ryubing Canary 的 headless 入口。旧 `Ryujinx.Headless.SDL2` 不需要这个参数 |
| `--root-data-dir <path>` | Ryujinx 数据目录 (keys, firmware, saves) |
| `--use-hypervisor false` | **macOS 必须加**，否则 Apple Hypervisor 报 Denied |
| `--disable-file-logging` | 日志输出到 stderr 而非文件 |
| `--enable-debug-logs` | 显示调试日志 |
| `--disable-docked-mode` | 手持模式 (对 smoke test 更简单) |
| `--ignore-missing-services` | 缺少 service 不崩溃 |
| `--enable-internet-connection` | 打开 guest 网络；headless 默认不打开，网络测试必须加 |
| `--graphics-backend Vulkan` | macOS 强制 Vulkan (MoltenVK), OpenGL 不支持 |
| `--fullscreen` | 全屏模式 |
| `--resolution-scale 2` | 2x 分辨率 |

## 日志策略

不要把 Ryubing 的 stdout/stderr 原样重定向到普通文件。当前 LunarNX 在 Ryubing 中会触发大量非致命 HLE 警告，例如 `WaitSynchronization(handleIndex: 0x00000000) = InvalidHandle`，原始模拟器日志可以在几分钟内增长到数 GB。

推荐方式：

- 默认运行 `scripts/run_ryubing_nro.sh`，脚本会丢弃 Ryubing stdout/stderr。
- 调试 LunarNX 逻辑时看 app 自己写的日志：`~/work/self/ryujinx-data/sdcard/switch/LunarNX/lunarnx.log`。
- 只有需要看模拟器自身事件时才用 `RYUBING_LOG_MODE=filtered`。
- 不建议使用 `RYUBING_LOG_MODE=raw`，除非只跑很短时间。

使用 `--send-test-rumble` 时，mock 日志应出现
`[INPUT] Sent deterministic test rumble`，LunarNX app 日志应出现
`[rumble] command left=25 right=50 lt=75 rt=100`。该验证只证明协议解析和
回调完成；实际 Joy-Con/Pro Controller 震动仍需 Switch 实机确认。

## Ryujinx 数据目录结构

```
~/work/self/ryujinx-data/
├── system/
│   └── prod.keys              # 密钥文件 (文本)
├── bis/system/Contents/
│   └── registered/            # 固件 NCA (重要: 每个 NCA 放在独立子目录)
│       ├── 0a02c03e....nca/
│       │   └── 00             # 实际 NCA 文件
│       ├── 0a932d55....cnmt.nca/
│       │   └── 00
│       └── ... (235个目录)
├── profiles/                  # 输入配置
├── games/                     # 游戏目录
└── sdcard/                    # 虚拟 SD 卡
```

**关键**：固件 NCA 文件必须放在 `{hash}.nca/00` 格式的子目录中，不能直接放在 `registered/` 下。这是 Ryujinx 的 `ContentManager.LoadEntries()` 的要求（它遍历子目录而不是文件）。

## 固件和密钥

请使用自己 Switch 导出的 `prod.keys` 和固件文件准备 Ryujinx 数据目录。

```bash
unzip Firmware.20.5.0.zip -d /tmp/fw
cd /tmp/fw
for f in *.nca *.cnmt.nca; do
    mkdir -p ~/work/self/ryujinx-data/bis/system/Contents/registered/"$f"
    cp "$f" ~/work/self/ryujinx-data/bis/system/Contents/registered/"$f"/00
done
```

## 构建 NRO

```bash
# Smoke test
make -f Makefile.switch.smoke

# 完整 LunarNX (需要预编译 borealis, mbedtls, srtp2 等)
make -f Makefile.switch
```

按当前 Makefile 语法编译:
- `build/switch-smoke/LunarNXSmoke.nro` → smoke test (204KB)
- `build/switch/LunarNX.nro` → 完整版

## 测试结果判断

### 通过标准
```
Application LoadApplication: Using Firmware Version: 20.5.0
Application LoadApplication: Loading as Homebrew.
Loader Start: Application Loaded: LunarNXSmoke v20.5.0 [64-bit]
Gpu PrintGpuInformation: Apple M2 (Vulkan v1.2.231)
```

### 实机启动前的 NRO BSS 检查

完整 NRO 在 Ryubing 中能打开，不代表 hbmenu 在真实 Switch 上一定能完成装载。
Ryubing 对静态内存映像的容忍度可能高于实机。当实机只显示通用的“软件已关闭”错误，
并且没有生成 `lunarnx.log`、Atmosphere crash report 或 fatal report 时，故障可能发生在
`main()` 之前，应优先检查 NRO 的 BSS，而不是继续简化首屏 UI。

2026-07-12 的实机启动故障由 `src/platform/switch_heap.c` 中 64 MiB 的静态
fallback heap 引起。它使当前 LunarNX NRO 的 BSS 从正常的约 20 MiB 增长到
84.4 MiB。NRO loader 必须在调用 libnx 初始化和 `main()` 之前为 BSS 保留并清零
内存，因此即使运行时 `svcSetHeapSize()` 本来可以成功，这块 fallback 仍会增加
装载压力。删除自定义 heap、恢复为 Moonlight-Switch 同样使用的 libnx 默认动态
heap 后，BSS 降至 20.4 MiB，实机能够正常打开并进入 Xbox 主机列表。

已确认的参考值：

| NRO | BSS |
|-----|-----|
| 修复后的 LunarNX | 20.4 MiB |
| 旧版可启动 LunarNX | 20.4 MiB |
| Moonlight-Switch | 20.6 MiB |
| wiliwili | 20.7 MiB |
| 带 64 MiB 静态 fallback 的 LunarNX | 84.4 MiB，实机无法打开 |

本项目将 32 MiB 作为 BSS 回归阈值。它是根据已知可启动产物设置的项目保护线，
不是 Switch 平台的通用硬上限。每次完整 Switch 构建后运行：

```bash
python3 tests/switch_nro_bss_test.py build/switch/LunarNX.nro
```

也可以直接检查 ELF：

```bash
aarch64-none-elf-size -A build/switch/LunarNX.elf | rg '^\.bss|^\.tbss'
aarch64-none-elf-nm -S --size-sort build/switch/LunarNX.elf | tail -n 30
```

注意：BSS 不存储在 NRO 文件主体中，因此加入大型未初始化静态数组后，`ls -lh`
显示的 NRO 文件大小可能几乎不变。必须检查 NRO header/ELF section，不能用文件
大小判断静态内存占用。

后续约束：

- 不要使用大型静态数组作为解码、网络或 heap 的 fallback buffer。
- 大型运行时缓冲区应从 libnx 动态 heap 分配，并在进入相关功能时再创建。
- 默认保留 libnx 的 `__libnx_initheap`，与 Moonlight-Switch 对齐。
- 如果确实需要自定义 heap，必须先说明实机依据，并保证 NRO BSS 回归测试通过。

### 网络测试结论

Ryujinx/Ryubing 的网络结果不能完全等价于实机 Switch：

- 不加 `--enable-internet-connection` 时，Ryujinx 会直接拦截 guest 网络，日志可见 `Guest network access disabled` 或 `DNS Blocked`，NRO 内通常表现为 `CURLE_COULDNT_RESOLVE_HOST`。
- 加上 `--enable-internet-connection` 后，旧 Headless 可以走到 DNS/TCP/TLS；GitHub API 曾返回 HTTP `403`，说明 guest 不是完全断网。但百度、B 站、QQ、Microsoft OIDC 等公网 HTTP/HTTPS 请求容易出现连接后 `0 bytes received` 超时。
- 旧 Headless 的本地 LAN HTTP 测试可返回 11B、16KB、50KB 响应，说明它不是完全不能 `recv()`；公网超时更像模拟器 guest 网络/HLE 与外部站点交互的问题。
- Ryubing Canary 1.3.333 明显更适合做网络流程测试。同一套 `LunarNXNetProbe` 在 Ryubing 中已验证：

| 用例 | 结果 |
|------|------|
| `baidu-http` | `result=0 status=200 downloaded=29506` |
| `baidu-https` | `result=28 status=200 downloaded=3074`，随后 10s 超时 |
| `bilibili-nav` | `result=0 status=200 downloaded=259` |
| `github-api` | `result=0 status=403 downloaded=252` |
| `qq-https` | `result=0 status=200 downloaded=18` |
| `microsoft-oidc` | `result=0 status=200 downloaded=1775` |

因此当前推荐：

- NRO 加载、首屏进入、日志链路：Ryubing 或旧 Headless 都可以。
- 网络/API 流程：优先用 Ryubing Canary。
- 最终网络结论：以真实 Switch 为准。

### 已知 HLE 限制

Ryujinx 系模拟器仍存在 `recv()` 大 buffer 的 HLE 问题：当 guest 侧 `recv()` buffer 达到 `32768` 字节时，旧 Headless 和 Ryubing Canary 都可能异常。

已验证结果：

| recv buffer | 结果 |
|-------------|------|
| `1024` / `8192` / `16384` / `32767` | 正常 |
| `32768` | 旧 Headless 可触发 `ArgumentOutOfRangeException`，Ryubing 可触发 `OverflowException` |

所以 Switch 版 libcurl 仍建议显式限制：

```cpp
curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 16384L);
```

这个设置不是公网超时的根因，但可以避开模拟器 HLE 的 32KB recv buffer 崩溃点。

### 串流启动页与 Activity 切换

2026-07-15 的一次 Ryubing 回归表现为：读取已保存登录后，日志停在
`[ui-main] MainActivity create ...`，还没有进入 Xbox 列表请求或串流启动，随后
Ryujinx 的 `HLE.GuestThread` 以 `SIGABRT` 退出。回归来自在 `MainActivity` 初次
构造时加入隐藏、可聚焦的全屏 detached overlay，并在其中预先创建
`ProgressSpinner`。Borealis 的 `Box::willAppear()` 会递归通知所有子视图，即使
overlay 当前是 `GONE`，因此隐藏 spinner 仍会在首页首帧布局阶段启动动画。

当前 UI 约束与 Moonlight-Switch / wiliwili 的播放器导航方式对齐：

- `MainActivity` 保持已验证的 `ScrollingFrame` 根节点，不在首页预建串流覆盖层。
- 用户按 Play 后再创建独立的串流启动 Activity；该页面可见并完成首次绘制后，
  才通过 `brls::sync` 排队启动网络 worker。
- 加载 spinner 属于目标 Activity，始终在可见生命周期内运行。
- 启动页与串流页使用 `brls::TransitionAnimation::NONE`，避免在 Ryubing/MoltenVK
  上为重页面额外创建 fade 动画工作。
- 启动页持有焦点并消费重复的 A；B 先取消/清理串流任务，完成后再返回首页。

相关结构回归测试：

```bash
python3 tests/stream_loading_activity_test.py
```

### 常见问题

| 日志 | 含义 | 处理 |
|------|------|------|
| `OpenGL is not supported on macOS, switching to Vulkan!` | 正常警告，自动切换 |
| `SendSyncRequest() = InvalidHandle` | 某 service 未找到。可能缺固件或 HLE 不支持 |
| `Guest network access disabled` / `DNS Blocked` | Headless guest 网络未开启 | 使用 `RYUJINX_ENABLE_INTERNET=1` 或手动加 `--enable-internet-connection` |
| `Blocking socket operations are not yet working properly. Expect network errors.` | Ryujinx BSD HLE 警告 | 常见警告；单独出现不代表请求一定失败 |
| `SetSocketOption ... Operation not supported` | 某些 socket option 未完整模拟 | 常见警告；结合 NRO 日志判断 |
| `recv()` 32KB 附近异常 | Ryujinx BSD HLE buffer 问题 | 限制 `CURLOPT_BUFFERSIZE` 到 `16384` 或更低 |
| `NPDM file not found, using default values!` | Homebrew 正常，无 NPDM |
| `ServiceXxx: Stubbed.` | 正常，HLE 桩实现 |
| `AppleHv: Unexpected result "Denied"` | 未加 `--use-hypervisor false` |

## 按键模拟 (未解决)

当前 smoke test (`src/tools/switch_smoke.c`) 支持按键检测，会打印 `BUTTON: <name>`。

尝试过的方法：
1. **osascript keystroke** — SDL2 不接收 AppleScript 合成事件
2. **CGEvent (CGEventPost)** — SDL2 不接收
3. **CGEventPostToPid** — 同样无效

SDL2 在 macOS 上通过 IOKit HID 直接读取键盘，绕过了 CoreGraphics 事件系统。

**可能的解决方案**：
- 使用 `AXUIElementPostKeyboardEvent` (Accessibility API)
- 创建一个虚拟 HID 设备 (需要 Kext/Dext, 复杂)
- 使用 Ryujinx 的 input profile 配置 + SDL2 虚拟手柄
- 用 Swift/Metal 直接注入 SDL2 事件循环
- 使用 `CGEventPost` 到 `kCGSessionEventTap` + 先让 Ryujinx 窗口获得焦点

## 编译/更新 Ryujinx

```bash
# 旧 Headless SDL2 构建方式
cd ~/work/self/ryujinx
export PATH="$HOME/dotnet:$PATH"
dotnet publish -c Release -r osx-arm64 \
  -p:DebugType=embedded \
  -p:SigningCertificate=- \
  --self-contained true \
  -o ./publish_headless \
  src/Ryujinx.Headless.SDL2
```

注意：
- 网络测试优先使用 Ryubing Canary release。它没有单独的 `Ryujinx.Headless.SDL2` 二进制，使用主程序 `Ryujinx --no-gui`。
- 如果从 Ryubing 源码构建，应构建 `src/Ryujinx`，然后用 `--no-gui` 运行。
- 需要 .NET 8.0 SDK (已在 `~/dotnet/`)
- 当前源码来自 `git.axenov.dev/Museum/ryujinx`
- 原版 `git.nadeko.net/ryujinx-mirror/Ryujinx_GreemDev` 的 Headless.SDL2 源码不完整
- `Directory.Packages.props` 可能需要补充 `Ryujinx.Graphics.Nvdec.Dependencies` 版本条目
