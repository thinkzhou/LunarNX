# PS / Xbox 视频解码策略分离设计

状态：已批准（实现中）
日期：2026-08-18
分支：refactor/ps-xbox-decoder-separation

## 1. 背景与问题

`6f55c77 perf(stream): separate Xbox and PlayStation video paths` 引入的共享
`VideoDecoder` 内部按 `video_path_` 分支，导致 Xbox 链路丢掉了 standalone
SPS/PPS 处理：

```diff
- if (!au.has_vcl) {                      // 所有路径：缓存 standalone 参数集
+ if (playstation_path && !au.has_vcl) {  // 只有 PS 缓存
```

配套还限死了两处：解码前 prepend 缓存参数集、`resetForKeyframe` 保留参数集。
Xbox 服务端在 encoder recovery / 码率变化 / keyframe 刷新时偶尔单独发送
SPS/PPS，新代码直接喂给 NVDEC/FFmpeg，造成解码状态不同步 → 周期性解码报错 →
触发 recovery → 表现为「玩一会掉一下然后恢复」。

设计规则（`docs/xbox_playstation_dual_streaming_design.md` §1、§3、§8）：Xbox 与
PlayStation 只在「完整编码访问单元 + 时间戳」边界汇合，不为 PS 改动稳定的 Xbox
路径。本次拆分落实该规则到解码策略层。

## 2. 目标

- Xbox 与 PS 的视频解码**策略**（AU 解析、standalone 参数集缓存、解码门控、
  reset 行为、H264 限制）各自独立拥有，共享代码中不再出现按 `video_path_`
  的运行时分支。
- 修复 Xbox regression：恢复 standalone SPS/PPS 缓存 + 解码前 prepend +
  `resetForKeyframe` 保留参数集。
- 传输层（Xbox=libpeer/WebRTC，PS=chiaki）已经分离，不在本次范围；渲染、音频、
  AV 同步、NVDEC 引擎是行为一致的基础设施，保持共享。

## 3. 设计

### 3.1 `VideoDecoder` 基类 —— 只保留 FFmpeg/NVDEC 基础设施

- 保留：`initialize` / `shutdown` / `flush` / `setCallback` / `setPerfStats` /
  `setVideoBackend` / `setVideoCodec` / `deliverFrame`。
- `initialize` 改为 virtual，供 Xbox 子类先做 H264-only 守卫。
- 新增受保护成员：
  - `decodeAccessUnit(data, len, timestamp, au, log_index, log)`：从现有
    `decode()` 抽出的 NVDEC / 软件 send+receive 循环（纯搬运，无策略）。
  - `reinitializeParser()`：`avcodec_flush_buffers` + parser 重建，供 reset 复用。
- 纯虚接口：`inspectAccessUnit()` / `decode()` / `resetForKeyframe()`。
- 基类**删除 `video_path_`**，不持有任何路径分支。
- 基类状态仅基础设施：`video_backend_` / `video_codec_` / `codec_ctx_` /
  `parser_` / `hw_device_ctx_` / `on_frame_` / `perf_` / `initialized_` /
  `error_log_count_`。

### 3.2 `XboxVideoDecoder : VideoDecoder`

- `inspectAccessUnit()` → `inspectXboxH264AccessUnit`（免分配 fast path）。
- `decode()` 策略：
  - 复用调用方传入的 `VideoAccessUnitInfo`，否则自己解析。
  - standalone SPS/PPS（`!au.has_vcl`）→ **缓存并 return true（恢复）**；
    收到新 SPS 清空旧缓存。
  - 门控：`seen_sps && seen_pps && au.has_random_access` 才开解码；未就绪的
    VCL 丢弃等待。
  - 解码前把缓存参数集 prepend 到首个 VCL（**恢复**）。
  - 调基类 `decodeAccessUnit` 完成实际解码。
- `resetForKeyframe()` → `parameter_sets_pending_ = !parameter_sets_.empty()`
  （**恢复**）。
- `initialize()` → 先守卫 `video_codec_ != H264` 返回 false；复位策略状态后调
  基类。
- 策略状态：`decoder_ready_` / `seen_sps_` / `seen_pps_` / `parameter_sets_` /
  `parameter_sets_pending_` / `wait_log_count_`。

### 3.3 `PsVideoDecoder : VideoDecoder`

- `inspectAccessUnit()` → `inspectVideoAccessUnit(video_codec_, ...)`（H264+HEVC、
  NAL 诊断串）。
- `decode()` / `resetForKeyframe()` 与 Xbox 同形状，额外支持 HEVC：
  - 门控需要 `seen_vps && seen_sps && seen_pps`；缓存清空条件含 `has_vps`。
- 无 H264-only 守卫。
- 策略状态多一个 `seen_vps_`。

> 注：两个子类的 decode/gate/reset 策略体初始近乎相同（均恢复被验证过的 0.1
> 统一行为）。这是**有意重复**：隔离优先于 DRY，未来任一链路单独调策略只改自己
> 的类。

### 3.4 `StreamBackendProvider`

`createVideoDecoder()` 增加 `VideoPipelinePath` 参数；默认实现按路径返回
`XboxVideoDecoder` 或 `PsVideoDecoder`。

### 3.5 `MediaPipeline`

- `enqueueVideoPacket()`：`packet.access_unit = video_decoder_->inspectAccessUnit(...)`
  —— 委托给 decoder，删除 `video_path_` 分支。
- 删除 `video_path_` / `video_codec_` 成员（仅构造期使用）；删除
  `video_decoder_->setVideoPath(...)`。
- `MediaPipelineOptions.video_path` 保留，作为构造期选择器传入
  `createVideoDecoder`。
- 队列 / hard recovery / DirectLowLatency / 调度配置均不改。

### 3.6 `ps_media_replay.cpp`

设 `options.video_path = PlayStation`，修复 HEVC 回放被 Xbox H264-only 守卫
挡掉的问题（6f55c77 引入）。

## 4. 文件改动

- `src/stream/video_decoder.h` / `.cpp`：拆为基类 + 两个子类（同文件，不加新文件，
  构建清单零改动）。
- `src/stream/stream_backend_provider.h` / `.cpp`：`createVideoDecoder(path)`。
- `src/stream/media_pipeline.h` / `.cpp`：委托 inspect、删路径成员。
- `src/tools/ps_media_replay.cpp`：设 PS path。
- 测试：
  - `tests/xbox_video_fast_path_test.py`：改为断言 XboxVideoDecoder 拥有
    fast-path + 恢复的 standalone 参数集缓存。
  - `tests/ps_h264_startup_gate_test.py`：改为断言 PsVideoDecoder 拥有门控/
    缓存行为。
  - `tests/ps_hevc_support_test.py`：改为断言 PsVideoDecoder 支持 HEVC。
- 预期不动的测试：`software_video_backend_test`、`media_pipeline_async_test`、
  `media_pipeline_scheduling_test`、`xbox_stream_session_order_test` 等。

## 5. 验证

- 桌面增量编译改动 TU；`make -f Makefile.desktop video_codec_tests` 跑 C++ 测试。
- 运行受影响及相邻的 Python 文本断言测试。
- Switch：本地无 devkitPro，`#ifdef __SWITCH__` 段落格外复核；建议
  `scripts/build_switch.sh` 容器构建确认。
- 真机：用户实测 Xbox 串流，确认 ~30s 周期性掉帧消失；PS 串流无回归。

## 6. 非目标

- 不改传输层、渲染、音频、AV 同步、调度配置。
- 不引入完整双 pipeline（不复制队列/worker/音频）。
- 不调整 Xbox 的免分配 fast-path 解析器本身。
