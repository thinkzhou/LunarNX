# rho — 本地视频播放器 & 模拟器解码管线验证

## 目标

在 Switch 模拟器（Ryujinx）上验证视频解码 + 渲染管线是否能正常工作，包括：

- NVDEC 硬件 H.264 解码
- deko3d GPU 渲染（NV12→RGB，zero-copy NVDEC→GPU）
- AAC 软件音频解码 + audren 输出
- 帧率是否达标

## 文件位置

| 文件 | 说明 |
|------|------|
| `src/rho/rho_main.cpp` | rho 主程序 |
| `lib/switch/libmpv_deko3d.a` | wiliwili 的 deko3d 补丁版 mpv（未使用，保留备用） |
| `lib/switch/libuam.a` | deko3d shader 编译器（mpv 依赖，保留备用） |
| `lib/switch/include/mpv/` | 补丁版 mpv 头文件（保留备用） |

## 构建

```bash
# Switch 版（需要 Docker + devkitPro）
docker run --rm --platform linux/amd64 \
    -v $(pwd):/work -w /work \
    devkitpro/devkita64:latest bash -lc '
export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
# 先构建 borealis
cd /work/lib/borealis
cmake -S . -B build_switch \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
    -DPLATFORM_SWITCH=ON -DUSE_DEKO3D=ON -DBOREALIS_USE_DEKO3D=ON \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build_switch --target borealis -- -j$(nproc)
# 构建 rho
cd /work
make -f Makefile.switch rho -j$(nproc)
'
```

输出：`build/switch/Rho.nro`（约 20MB）

## 运行

```bash
# Ryujinx headless 模式
/tmp/ryubing-canary-1.3.333/Ryujinx.app/Contents/MacOS/Ryujinx \
  --no-gui \
  --root-data-dir ~/work/self/ryujinx-data \
  --disable-docked-mode \
  --ignore-missing-services \
  --use-hypervisor false \
  build/switch/Rho.nro
```

默认读取 `sdmc:/test_stream.mp4`（对应 `~/work/self/ryujinx-data/sdcard/test_stream.mp4`）。

生成测试视频：
```bash
ffmpeg -y -f lavfi -i "testsrc=duration=10:size=1280x720:rate=30" \
  -f lavfi -i "sine=frequency=440:duration=10" \
  -c:v libx264 -preset ultrafast -pix_fmt yuv420p \
  -c:a aac -shortest /tmp/test_stream.mp4
```

## 验证历程

### 方案 1：VideoDecoder(NVDEC) + VideoRenderer(deko3d)

复用了项目现有的视频解码和渲染组件。

**结果：解码成功，渲染失败（黑屏）**

- NVDEC 初始化成功，`avcodec_open2 NVDEC done`
- 解码输出格式为 `AV_PIX_FMT_NVTEGRA`（format=228），确认走了硬件解码路径
- `render()` 成功，`present()` submit 正常
- 但画面始终黑屏

**分析：** nanovg 渲染可见（红色方块测试通过），但 deko3d 视频渲染不可见。根因是 Ryujinx 模拟器不支持
`DkImageFlags_UsageVideo`（Tegra X1 专用 flag），导致 NVDEC 输出的 NvMap 纹理在 GPU 侧无效。

### 方案 2：mpv + deko3d 补丁（wiliwili 路线）

从 wiliwili 的 release 下载了预编译的 deko3d 补丁版 mpv。

**结果：无法使用**

编译成功，mpv 初始化成功，但 mpv 内部线程在 Ryujinx 中全部卡在
`WaitSynchronization(handleIndex: 0x00000000) = InvalidHandle`（HLE 同步原语不支持）。
wiliwili 本身在模拟器中也只能显示 borealis UI，无法播放视频。

### 方案 3：软件解码 + nanovg 显示

使用 FFmpeg 软件 H.264 解码 + swscale YUV→RGBA + nanovg 图像显示。

**结果：视频正常播放**

- H.264 软件解码：正常
- YUV420P → RGBA 转换（swscale）：正常
- nanovg 图像显示：正常
- 帧率控制：~28fps（~33ms/帧）
- EOF 循环播放：正常

### 方案 4：音频解码 + audren 输出

使用 FFmpeg 音频解码 + swresample（→48kHz s16 立体声）+ AudioPlayer（audren）。

**结果：PCM 音频正常播放，AAC 音频解码在模拟器中 segfault**

- audren 初始化成功（`audio=yes`，48kHz stereo）
- AudioPlayer 复用 `src/stream/audio_player.cpp`（与 LunarNX 主程序相同的 audren 实现）
- 参考 Moonlight-Switch 的方式：Switch 上全部用 audren（`AudrenAudioRenderer.cpp`），SDL2 仅用于桌面
- AAC：第一个音频包传入解码器即崩溃——`avcodec_send_packet` 内部 segfault，无 FFmpeg 错误日志
- PCM（`pcm_s16le`）：完全正常，解码、swresample、audren 播放全链路通畅
- **确认是 Ryujinx 对 FFmpeg AAC 解码器的 ARM NEON SIMD 指令模拟不完善**（PCM 不依赖 NEON，不崩溃）

### 方案 5：NVDEC copy-out + nanovg 显示（当前方案）

使用 FFmpeg NVTEGRA 解码，通过 `av_hwframe_transfer_data` 得到 CPU NV12，再用
swscale 转为 RGBA 并交给 nanovg。该路径避开 Ryubing 不支持的 NVDEC external
storage，同时仍由 NVDEC 完成 H.264 解码。

FFmpeg NVTEGRA 状态区初始化修复后：

- 720p30 稳定 30fps，平均视频处理约 2ms/帧。
- 1080p60 稳定 58-60fps，平均视频处理约 5-7ms/帧。
- packet 与输出 frame 保持 1:1，没有隔帧 `AVERROR_UNKNOWN`。
- 测试画面未见历史帧拖影。

## 当前最终状态

```
[MP4 文件] → avformat 解复用 → 视频: FFmpeg NVDEC → CPU NV12 → swscale RGBA → nanovg
                              → 音频: FFmpeg PCM 解码 → swresample → AudioPlayer(audren) (正常)
```

- 视频已验证 720p30 和 1080p60 实时播放
- 音频正常播放（PCM 格式，AAC 在模拟器崩）
- 1280x720 视频 + 44100Hz 单声道音频

## 关键结论

| 组件 | 模拟器 | 真机（预期） |
|------|--------|-------------|
| NVDEC 解码 + CPU copy-out | 正常工作 | 待实机验证 |
| deko3d `UsageVideo` flag | **不支持** | 正常工作 |
| mpv 多线程 | **HLE 同步原语不支持** | 正常工作 |
| FFmpeg H.264 软件解码 | 正常工作 | 正常工作 |
| FFmpeg AAC 解码 | **segfault** (疑似 NEON) | 正常工作 |
| audren 音频输出 | 初始化正常 | 正常工作 |
| nanovg 渲染 | 正常工作 | 正常工作 |

**模拟器可验证 NVDEC + CPU copy-out，但不能证明 NVDEC external storage + deko3d
零拷贝路径可用。** 最终硬件兼容性和性能仍以真实 Switch 为准。

## 已知限制

1. 音频测试片使用 PCM；AAC 在模拟器中仍可能崩溃
2. YUV→RGBA 是 CPU 转换（swscale），性能不如真机零拷贝 GPU 路径
3. 视频文件完整加载到内存，大文件需要流式读取

## 参考资料

- Moonlight-Switch 音频实现：`audren`（`AudrenAudioRenderer.cpp`），switch 上用 `#ifdef __SWITCH__`，桌面用 `#ifdef __SDL2__` 的 SDL
- wiliwili 音频实现：mpv 内置（`audio-channels = stereo`），mpv 内部处理音频输出
- 我们的 `AudioPlayer`：`src/stream/audio_player.cpp`，与 Moonlight 相同的 audren 方案
