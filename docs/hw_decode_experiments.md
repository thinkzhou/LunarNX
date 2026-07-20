# 硬解实验记录

> 目标：在 Ryubing 模拟器上验证 NVDEC 硬解、拷回渲染和 NvMap
> 零拷贝 deko3d 渲染，并保留三种可切换的视频后端供真机验证。

## 背景

- 软解（FFmpeg H.264 software + sws_scale RGBA + nanovg）：模拟器可用
- 半硬解（NVDEC + `av_hwframe_transfer_data` + sws_scale RGBA）：模拟器可用
- 全硬解零拷贝（NVDEC + NvMap external storage + deko3d）：模拟器可用
- 三条路径均保留为用户配置；真实 Switch 仍是最终兼容性和性能标准

## 环境

- 模拟器：Ryubing Canary 1.3.333（基于 Ryujinx 1.1.x 分支）
- 固件：已安装（235 NCA files in `bis/system/Contents/registered/`）
- 工具链：devkitPro Docker (`devkitpro/devkita64:20251117`)
- 测试视频：`sdmc:/test_stream_pcm.mp4`（720p H.264 + PCM 音频）

---

## 实验 1: NVDEC 解码 + av_hwframe_transfer_data + nanovg 显示

### 目标
验证 NVDEC 在 Ryujinx 上能解码出正确像素，通过 `av_hwframe_transfer_data` 拷贝到 CPU 后再用现有 nanovg 路径显示。

### 方法
1. 在 rho 中添加 `NvdecDecoder` 类，使用 `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_NVTEGRA)`
2. 解码后用 `av_hwframe_transfer_data` 将 `AV_PIX_FMT_NVTEGRA` 帧转为标准 NV12 CPU 帧
3. 复用现有 sws_scale + nanovg RGBA 显示路径

### 预期
- 如果 NVDEC 解码正常 → 画面正常显示，证明 NVDEC 输出有效
- 如果 NVDEC 解码正常但 transfer 失败 → 需要排查 hwframe_transfer 实现
- 如果 NVDEC 初始化失败 → 排查 hwdevice_ctx_create 是否依赖固件/tracing

### 结果（第 3 轮 — 无 parser + direct send_packet + drain-on-failure）

日期：2026-07-09

**结论：NVDEC + av_hwframe_transfer_data 在 Ryujinx 上完全可用！** ✅

- `extra_hw_frames=16` + 去掉 parser + send 失败时 drain 已有帧
- 用户确认画面正常显示，视频正常播放循环
- 之前日志分析有误：只打印前 5 帧 `(frame_count <= 5)`，后续帧虽然成功解码但无日志输出

### 关键结论（实验 1 最终）

| 步骤 | 状态 |
|------|------|
| `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_NVTEGRA)` | ✅ 正常 |
| `avcodec_open2` with `pix_fmt=228` | ✅ 正常 |
| `avcodec_receive_frame` → NVTEGRA hw frame | ✅ 正常 |
| `av_hwframe_transfer_data` → NV12 CPU frame | ✅ 正常 |
| `sws_scale` NV12→RGBA | ✅ 正常 |
| nanovg 显示 | ✅ 正常（用户确认画面正常） |

**NVDEC 解码 + av_hwframe_transfer_data 拷贝路径在 Ryujinx 上完全验证通过。**

## 实验 2: NVDEC 解码 + deko3d NV12→RGB 渲染（非零拷贝）

### 目标
验证不用 external storage / UsageVideo 的 deko3d NV12→RGB 纹理渲染路径。

### 方法
1. NVDEC 解码 + av_hwframe_transfer_data → NV12 CPU 帧（已验证）
2. 创建标准 deko3d DkMemBlock（不用 external storage）和 DkImage（不用 UsageVideo）
3. dkCmdBufCopyBuffer 把 NV12 数据拷贝到 deko3d 纹理
4. 用现有 NV12→RGB shader（romfs/shaders/texture_fsh.dksh）渲染

### 预期
- 如果 deko3d 正常创建纹理和渲染 → 画面正常，验证非零拷贝 deko3d 路径可行
- 如果黑屏 → 排查 deko3d 纹理上传/绑定

### 结果（第 1 轮 — 2026-07-09 代码审查）

- [x] 代码已就绪（`src/rho/rho_main.cpp` 中的 `Deko3DRenderer` + `NvdecDecoder`）
- [x] 修复 `pd` pool 大小: 1MB → 4MB（720p NV12 staging buffer 需要 ~1.35MB）
- [ ] 待编译测试

### 构建与运行

```bash
# 在 Docker 中构建
make -f Makefile.switch rho

# 运行
RYUBING_LOG_MODE=filtered ./scripts/run_ryubing_nro.sh build/switch/Rho.nro

# 查看日志
tail -f $RYUJINX_DATA/sdcard/switch/LunarNX/lunarnx.log

# 验证 NV12 数据（第一帧会自动保存）
ls -l $RYUJINX_DATA/sdcard/rho_deko3d_nv12.raw
```

### 关键观察点

1. NVDEC 初始化: 日志中应出现 `rho-nvdec: hwdevice_ctx_create done` 和 `avcodec_open2 done`
2. deko3d 初始化: 日志中应出现 `rho-dk: init ok`
3. NV12 上传: 日志中应出现 `rho-dk: saved NV12 frame` 和 `allocated persistent images`
4. 画面: 如果 deko3d NV12→RGB 渲染成功，应看到正常视频画面
5. 如果仍然黑屏但 NV12 数据正确，问题可能在 shader 纹理采样或 HLE 渲染

## 实验 3: NVDEC 状态区陈旧值与隔帧 `AVERROR_UNKNOWN`

### 现象

Ryubing 中 `avcodec_send_packet()` 曾几乎隔帧返回
`AVERROR_UNKNOWN (-1313558101)`。重试同一个 packet 后帧率可恢复，但画面会包含
历史物体位置，形成严重拖影；不重试时画面正确，但只能输出约一半帧。

原始 RGBA 帧对比进一步确认：

- 重试 `AVERROR_UNKNOWN` 的帧与 host FFmpeg 参考帧差异明显。
- 不重试的 Switch 帧与对应 PTS 的 host 参考帧逐字节一致。

因此 `AVERROR_UNKNOWN` 不是 EAGAIN，也不能作为同 packet 重试条件。LunarNX 只在
FFmpeg 明确返回 `AVERROR(EAGAIN)` 时 drain 并重试。

### 根因

wiliwili 的 NVTEGRA FFmpeg 补丁在 fence 完成后检查：

```c
nvdec_status->error_status != 0 || nvdec_status->mbs_in_error != 0
```

但提交新帧前没有初始化映射内存中的状态区。真实 NVDEC 会回写该区域；Ryubing
Canary 1.3.333 的 NVDEC HLE 并不总是完整回写，因而 FFmpeg 会读取上一帧或未初始化
的状态值并误报 `AVERROR_UNKNOWN`。

修复是在 `ff_nvtegra_start_frame()` 提交命令前只清零状态区：

```c
memset((uint8_t *)av_nvtegra_map_get_addr(input_map) + ctx->status_off,
       0,
       ctx->cmdbuf_off - ctx->status_off);
```

真实硬件仍会在 fence 完成前写入实际状态，所以真正的 NVDEC 错误不会被掩盖。

### 结果（2026-07-10）

| 测试 | 结果 |
|------|------|
| Rho 720p30, ~307 kbps | 稳定 30fps，frames=packets，约 2ms/帧 |
| Rho 1080p60, ~8 Mbps | 稳定 58-60fps，frames=packets，约 5-7ms/帧 |
| LunarNX mock 720p30 | 稳定 30fps，`decode_errors=0` |
| RTP/SRTP/H.264 完整性 | `missing/srtp_fail/h264_corrupt/rtp_queue_drop` 均为 0 |

三组测试均未再出现 `AVERROR_UNKNOWN`，截图未见历史帧拖影。

### 可复现构建

- 项目库：`lib/switch/libavcodec.a`
- 本地补丁：`tools/ffmpeg_switch_build/nvtegra-status-clear.patch`
- Docker 构建：`tools/ffmpeg_switch_build/build.sh`
- 固定环境：FFmpeg 7.1 + wiliwili 补丁 + `devkitpro/devkita64:20251117`

模拟器结果已经闭环，但真实 Switch 仍需最终验证 NVDEC 路径和长期稳定性。

---

## 实验 4: NVDEC NvMap 零拷贝 + Borealis 同步呈现

### 现象与根因

零拷贝最初可以解码出 NVTEGRA 帧，但表现为黑屏、彩色画面闪烁，或只提交数帧后停住。最终定位到三个独立问题：

1. NV12 fragment shader 缺少 transformation uniform buffer 绑定，导致输出为黑色。
2. 解码线程只在新帧到达时提交视频，和 Borealis 的 clear/swap 生命周期竞争，造成黑屏和彩屏交替。
3. 自定义逐帧 `DkFence` 在 Ryubing 中无法可靠完成；NvMap 映射和 descriptor 更新放在解码线程也会导致长期停顿。

### 最终路径

- 解码线程只 `av_frame_ref()` 保存最新 NVTEGRA 帧。
- `HardwareVideoView::draw()` 每个 Borealis UI frame 调用
  `StreamController::presentVideoFrame()`。
- NvMap external-storage 映射、image descriptor 更新和 deko3d submit
  全部在 Borealis 绘制线程执行。
- 无后处理时复用静态 direct command list，不使用逐帧 `DkFence`。
- 当前帧和 framebuffer ring 对应的近期退役帧持续持有，避免 NVDEC surface
  在 GPU 使用结束前被复用。
- shutdown、分辨率变化和静态 command list 重录时保留 `queue.waitIdle()`。

### 1080p60 结果（2026-07-10）

测试流：H.264 Constrained Baseline，1920x1080，60fps，约 12Mbps。

| 后端 | Ryubing FPS | Host CPU | 平均 render submit | 结果 |
|------|-------------|----------|-----------------------|------|
| `hardware_zero_copy` | 通常 54-60，偶发约 47 | 约 213-229% | 0.02-0.04ms | 正常，无闪烁 |
| `hardware_copy_out` | 约 55-60 | 约 248-254% | 0.69-0.70ms | 正常 |
| `software` | 约 37-57，波动明显 | 约 235-266% | 0.84-0.92ms | 可用但吞吐较弱 |

三种后端测试中，`missing`、`srtp_fail`、`decode_errors`、
`h264_corrupt` 和 `rtp_queue_drop` 均为 0。当前 `avg_decode_ms` 只统计
`avcodec_receive_frame()` 和部分 copy-out 时间，不包含所有可能发生在
`avcodec_send_packet()` 内的解码工作，因此不能用于比较三种后端的完整解码成本。

Ryubing host CPU 仅适合做相对比较，真实 Switch 上仍需验证 720p60、1080p30、
1080p60 的稳定帧率、功耗、温度和长时间 surface 生命周期。
