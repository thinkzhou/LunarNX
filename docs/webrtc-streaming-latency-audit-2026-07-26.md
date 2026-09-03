# WebRTC 串流低延迟与抗波动审计报告

> 历史状态说明（2026-09）：本文是 2026-07 的静态基线。此后当前实现已经
> 引入独立高频输入采样、网络路径估计、有界媒体队列、NACK/PLI 恢复、
> 自适应 REMB 码率和动态 Realtime/Balanced/Recovery 延迟模式。下方问题与
> Phase 列表用于解释演进背景，不应直接视为当前待办；当前状态以
> [TECHNICAL_PLAN.md](TECHNICAL_PLAN.md) 和代码为准。

- 日期：2026-07-26
- 范围：LunarNX 当前工作区中的 Xbox WebRTC、RTP/SRTP、H.264/Opus、解码、A/V sync 和 Switch 零拷贝渲染链路
- 目标：评估局域网 Xbox 串流能否达到约 20ms 延迟，并在网络波动时减少掉帧、冻结和累计延迟
- 状态：静态代码审计与现有回归测试已完成；尚未修改运行代码

## 1. 结论摘要

LunarNX 已经具备完整的 WebRTC 媒体链路，RTP/H.264 重组、NACK/PLI、音频重排、NVDEC 和 Deko3D 零拷贝等基础方向是正确的。在干净局域网中，当前实现有机会获得较低延迟。

但如果目标是“客户端新增延迟稳定低于 20ms”，当前实现仍有四个最高优先级问题：

1. **WebRTC 收包处理被绑定到 16ms 输入循环。** RTP、RTCP、DTLS 和 NACK 不是到达后立即处理，而是大约每 16ms 批量 drain，一次轮询本身可引入 0–16ms、平均约 8ms 的等待。
2. **libpeer RTP FIFO 和 MediaPipeline 编码队列过大。** 短暂拥塞可能演变为持续处理陈旧数据，形成数秒级累计延迟。
3. **缺包 frame 会阻塞后续 frame 60–180ms。** NACK 仅发送一次，没有重试和 frame deadline，丢包时容易出现明显冻结。
4. **没有持续媒体 RTT 和真正的自适应码率。** RTCP RR 的 jitter 为 0，REMB 使用固定 profile bitrate，网络波动时只能被动等待丢包和 IDR 恢复。

当前主要矛盾不是单个 RTP 包解析慢，也不是 NVDEC 单帧解码时间过长，而是：

```text
16ms 批量收包
  → RTP/编码队列积压
  → 缺包 frame 队头阻塞
  → NACK/重传处理继续受到轮询延迟
  → 继续播放陈旧数据或等待 IDR
  → 卡顿、跳帧和操作延迟增长
```

因此，后续优化应先处理网络调度、队列年龄和丢包恢复，再继续微调 shader、decoder surface 数量或单帧解码性能。

## 2. 当前媒体链路

视频路径：

```text
Xbox UDP socket
  → ICE / DTLS / SRTP
  → libpeer RTP FIFO
  → PeerManager::onVideoTrack
  → VideoRtpJitterBuffer
  → H.264 Access Unit
  → MediaPipeline encoded video queue
  → FFmpeg / NVDEC
  → VideoRenderer latest-frame handoff
  → Borealis draw / Deko3D present
  → Switch 屏幕
```

音频路径：

```text
Xbox Opus RTP
  → libpeer RTP FIFO
  → AudioPacketReorder
  → MediaPipeline audio queue
  → Opus decoder / PLC
  → Audren wave buffers
  → audio master clock
```

## 3. 20ms 目标的定义

完整的 Xbox input-to-photon 延迟包括：

```text
手柄采样
  → DataChannel 发送
  → Xbox 游戏处理与渲染
  → Xbox 捕获和编码
  → 网络
  → Switch 解码
  → GPU submit
  → VSync 和屏幕扫描
```

60fps 单帧周期已经是 16.67ms，因此“完整端到端延迟稳定低于 20ms”是非常激进的目标。当前更适合先定义：

- **客户端新增延迟**：从 Switch 收到 RTP 到 Deko3D present submit。
- **网络恢复时间**：从发现缺包到重传修复或新 IDR 恢复。
- **队列年龄**：各级队列中最旧对象距当前时间的差值。
- **完整 input-to-photon**：后续使用高速摄影、光电测试或可关联的输入/画面标记测量。

建议第一阶段目标：

| 指标 | 建议目标 |
|---|---:|
| LAN RTT | p95 小于 5ms |
| RTP arrival → present submit | p50 小于 8ms，p95 小于 16.7ms |
| libpeer RTP queue age | 稳态 p95 小于 8–10ms |
| MediaPipeline video queue age | 稳态 p95 不超过 1 帧 |
| 客户端新增延迟 | p95 尽量小于 20ms |
| 稳态显示间隔 | p95 不超过 33.3ms |
| 0.5% 随机丢包 | 不产生累计延迟，恢复 p95 小于 100ms |
| 带宽恢复后 | 不回放旧 backlog，数秒内恢复稳定码率 |

统计必须至少同时报告 p50、p95 和 p99，不能只看平均值。

## 4. 已确认的正确实现

### 4.1 RTP/H.264 基础处理

`src/webrtc/video_rtp_jitter_buffer.cpp` 已处理：

- RTP header extension 和 padding。
- 16-bit sequence wrap-around。
- 乱序和重复包。
- 单 NAL、FU-A 和 STAP-A。
- Access Unit、packet 数量和 payload 字节上限。
- 缺包 Generic NACK。
- 不完整帧超时后的 IDR recovery。

当前工作区还减少了大帧重复 assembly，并避免普通 RTP recovery 直接等待整个 GPU idle。这两项对 1080p60 和低延迟都有正面作用。

### 4.2 音频重排与 PLC

`src/stream/audio_packet_reorder.h` 当前使用：

- 4 包 reorder window。
- 最多 3 帧 Opus PLC。
- sequence gap 大于 64 时重置。

该设计适合处理轻微乱序和少量丢包，结构本身没有发现明显正确性问题。

### 4.3 Switch 零拷贝生命周期

`src/stream/video_renderer.cpp` 当前采用：

- `render()` 只保留最新 pending frame。
- `present()` 在 Borealis draw 生命周期中执行。
- command ring fence 退休后才释放 submitted AVFrame。
- 普通 present 和普通 RTP recovery 不调用全局 `waitIdle()`。
- shutdown 时才完整等待 GPU 并释放资源。

这比在解码线程直接提交 swapchain 或每次 recovery 都执行 `waitIdle()` 更适合稳定低延迟。

## 5. 主要问题

### P0：16ms 主循环同时限制输入和 WebRTC 收包

`src/app/xbox_stream_session.cpp` 中：

```cpp
constexpr std::chrono::milliseconds kInputPollInterval{16};
```

稳态循环执行顺序为：

```text
读取并发送手柄状态
→ transport_.processEvents()
→ 获取统计和处理恢复
→ sleep 到下一个 16ms input tick
```

相关位置：

- `src/app/xbox_stream_session.cpp:15`
- `src/app/xbox_stream_session.cpp:516`
- `src/app/xbox_stream_session.cpp:795`
- `tests/xbox_stream_session_order_test.py:48`

稳态串流期间没有独立网络线程持续调用 `processEvents()`，因此一个刚好在本轮处理结束后到达的 RTP 包，可能等待接近 16ms 才进入 libpeer 和 jitter buffer。

影响包括：

- 普通 RTP 包增加 0–16ms，平均约 8ms 调度等待。
- RTP 以 burst 进入 jitter buffer 和 decoder。
- 更容易错过当前 VSync，额外等待一帧。
- sequence gap 最多晚 16ms 才被发现。
- NACK 重传包回来后又可能等一个 16ms tick。
- 一次低 RTT NACK recovery 仍可能被轮询放大到 16–32ms 以上。
- 手柄采样自身也有平均约 8ms、最坏约 16ms 的等待。

`src/app/web_rtc_transport.cpp` 和 `src/app/xbox_channel_manager.cpp` 中另外两处 16ms sleep 只用于建连和握手，不影响稳态媒体延迟。真正的问题是稳态 transport processing 与 input heartbeat 共用一个 16ms 时钟。

### P0：libpeer RTP FIFO 会保留旧包并丢弃新包

`lib/libpeer/src/peer_connection.c` 当前配置：

```cpp
PEER_CONNECTION_MAX_PACKETS_PER_LOOP = 128
PEER_CONNECTION_RTP_QUEUE_CAPACITY = 4096
PEER_CONNECTION_RTP_DECODE_BUDGET = 64
```

每个 queue slot 使用 `CONFIG_PACKET_BUFFER_SIZE=2048`。单连接固定容量约 8MiB；按 `CONFIG_MTU=1300` 计算，可容纳约 5.3MiB RTP 数据，在常见串流码率下可能对应数秒媒体。

队列风险：

- 它是 FIFO，不按实时性和包年龄淘汰。
- 队列满时拒绝新到包，旧包仍继续被处理。
- 每轮最多接收 128 包、解码 64 包，接收和消费预算不对称。
- `PeerManager::processEvents()` 虽然最多 drain 8 次，但总时间限制为 3ms。
- `media_enabled=false` 时 RTP 仍入队，只是不解码。
- 重新启用媒体时没有清理过龄 RTP。

这会把短暂处理波动转化为持续播放旧内容，是最直接的累计延迟来源之一。

注意：不能通过删除 legacy libpeer packet-buffer fix 或缩小 `CONFIG_PACKET_BUFFER_SIZE` 解决。`CONFIG_MTU` 是 packetization size，`CONFIG_PACKET_BUFFER_SIZE` 是入站 UDP 接收缓冲。需要调整的是队列数量、包年龄和拥塞丢弃策略。

### P0：MediaPipeline 编码队列过大

`src/stream/media_pipeline.cpp` 当前上限：

```cpp
video: 2048 个 Access Unit / 32 MiB
audio: 512 个 packet / 4 MiB
```

视频队列中的对象已经是完整 H.264 Access Unit。按 60fps 计算，2048 个 AU 理论上可覆盖约 34 秒；通常 32MiB 字节限制会更早触发，但仍足以保存数秒数据。

当前只有达到上限时才会：

- 清空视频队列。
- 标记 decoder recovery。
- 请求新 IDR。

达到上限之前会继续按 FIFO 解码旧 AU，因此可能出现“画面仍然连续，但操作延迟越来越高”。

Moonlight 的 decode-unit queue 容量为 15，溢出后清空并请求 IDR。LunarNX 不必完全照搬，但应从按包数/字节限制改为按 frame age 和毫秒限制。

### P0：jitter buffer 存在 60–180ms 队头阻塞

`VideoRtpJitterBuffer` 当前：

- 默认 hold 为 120ms。
- 根据 `ice_rtt_ms` 调整为 `clamp(2 * RTT + 20ms, 60ms, 180ms)`。
- 只从 `frames.front()` 开始 drain。
- front frame 不完整时，即使后续 frame 已完整，也会停止处理。

局域网 RTT 较低时，NACK 通常应在数毫秒内完成。最低 60ms hold 已相当于约 3–4 个 60fps 帧周期，180ms 则约 11 帧。

当前策略偏向尽量保全每个 frame，而低延迟串流更需要：

```text
短 deadline 内尝试重传
→ 超时立即放弃损坏 reference chain
→ 保持上一张完整画面
→ 尽快请求和切换到新 IDR
```

### P0：NACK/PLI/RTCP 反馈不足

当前 NACK 在发现 sequence gap 时只发送一次，没有：

- NACK retry schedule。
- missing packet 首次发现时间。
- 每个 frame 的 recovery deadline。
- 重传成功率和恢复耗时。
- 已恢复 missing set 的长期跟踪。

关键帧恢复约按 1 秒节流。RTCP RR+REMB 也约每秒发送一次，但：

- RR jitter 固定为 0。
- REMB 使用静态 stream profile bitrate。
- 没有基于 loss、RTT、queue age 或 decoder load 的 congestion controller。

带宽骤降时，客户端仍声明原配置码率，只能依赖丢包、NACK、PLI 和新 IDR 被动恢复。

### P1：当前 RTT 不是持续媒体 RTT

`network_rtt_ms` 来源于 ICE candidate pair 的 STUN connectivity check RTT，不是基于 RTP/RTCP 的持续网络估计。

当前代码把它用于 jitter hold 调整，但网络在连接后发生变化时，该值未必及时反映真实媒体路径，因此所谓 adaptive hold 可能长期使用旧 RTT。

### P1：FPS 和丢包统计可能掩盖卡顿

`XboxStreamSession::createPeerCallbacks()` 对视频和音频均调用：

```cpp
perf_.recordPackets(1, 0);
```

因此 `PerfOverlay` 使用的 `packets_lost` 可能长期为 0。另一个 StreamOverlay 使用 `rtp_video_missing_packets`，两套统计口径不一致。

`video_frames` 也不是实际 displayed frames。只要 frame 成功写入 renderer 的 latest-frame slot 就会计数，而该 frame 可能在 Borealis 下一次 present 前被更新的 frame 替换。

当前缺少：

- RTP arrival → AU complete。
- AU complete → decoder start。
- decoder output → renderer queue。
- renderer queue → command submit/completion。
- unique displayed frame ID。
- frame age 和各级 queue age。
- pending frame replacement 数量。
- NACK retry/success 和 PLI→IDR 时间。

### P1：decoder 输出 timestamp 可能错配

VideoDecoder 创建 AVPacket 后未设置 PTS/DTS，`avcodec_receive_frame()` 返回的 frame 直接使用本次 `decode()` 输入 AU 的 timestamp。

在 Xbox Constrained Baseline、无 B-frame且 decoder 立即输出时通常可以工作，但以下情况可能错配：

- NVDEC 内部保留 frame。
- `avcodec_send_packet()` 返回 EAGAIN 后先 drain 旧 frame。
- 一个输入产生多个输出。
- 未来编码 profile 出现 frame reorder。

Switch 硬解还设置了 `extra_hw_frames=16`。它不一定造成延迟，但必须通过真机的 surface pipeline 深度和 frame age 测量判断，不能仅依靠 decode throughput。

### P1：A/V sync 有策略但缺少明确显示 deadline

当前音频是 master clock：

- 视频领先超过 50ms时返回 Wait。
- 视频落后超过 200ms 时 Drop。
- 音频 500ms 未更新后退回 wall clock。

但 Wait 分支只计算和记录 `wait_ns`，实际不会阻塞 decode worker。这样能避免填满 encoded queue，但最终显示时间只依赖 latest-frame handoff 和 Borealis draw cadence，没有逐帧明确 display deadline。

`docs/TECHNICAL_PLAN.md` 中仍描述“>5ms 丢帧、>2ms sleep”，已经与当前代码不一致。

### P1：NvMap mapping cache 缺少运行期 eviction

当前 zero-copy mapping cache 最大为 32，达到上限后拒绝新 frame。正常运行期间没有 eviction，仅 shutdown 时 `waitIdle()` 并清空。

反复 decoder reset、surface pool 重建、分辨率变化或长时间串流可能不断添加 mapping，最终达到 32 上限。该问题需要真实 Switch 长时间运行验证，Ryubing 不能作为最终证据。

### P1：音频缓冲偏向抗抖而非低延迟

Switch Audren 当前：

- 每个 wave buffer 聚合 5 个 Audren frame。
- 一共 5 个 wave buffer。
- queued audio 超过 500ms 才拒绝继续写入。

这能增加抗抖能力，但与客户端新增延迟 20ms 的目标冲突。视频采用 latest-frame 策略而音频允许深队列，也可能导致视频长期等待或丢弃以追随音频 master clock。

## 6. 修改计划

### Phase 0：补齐测量和故障注入

在改动实时策略前，先建立可复现反馈回路。

为 `tools/mock_xbox/mock_xbox_server.py` 增加：

- 固定 delay。
- random jitter。
- random loss。
- burst loss。
- packet reorder。
- duplicate。
- bandwidth throttle。
- bandwidth step-down / recovery。

为每个视频 frame 记录：

```text
RTP arrival
→ AU complete
→ encoded queue push/pop
→ decoder output
→ renderer pending
→ Deko3D submit
→ command-ring completion
```

新增统计：

- queue length 和 oldest age。
- frame age。
- displayed unique FPS。
- pending replacement 数量。
- NACK count/retry/success。
- PLI→IDR recovery time。
- NvMap mapping generation 和 cache size。
- p50/p95/p99。

### Phase 1：解耦 16ms 输入时钟与 WebRTC transport

推荐长期结构：

```text
Transport owner thread
  → 独占 libpeer
  → socket/event 驱动或 1–2ms 最大等待
  → 持续处理 RTP/RTCP/DTLS/SCTP
  → 接收其他线程提交的 input/control/feedback command

Input scheduler
  → 保留 16ms heartbeat
  → 状态变化可立即提交

Media workers
  → 接收完整 AU/Opus packet
  → 不拥有 libpeer
```

libpeer 不应由多个线程并发调用。专用 transport 线程应成为唯一 owner，其他线程通过线程安全队列提交命令。

过渡验证方案可以先把 session loop 调整为 1–2ms cadence，同时单独判断 `next_input_tick`，保持手柄每 16ms 发送。该方案便于验证延迟收益，但必须测量 Switch CPU 占用，不能长期依赖高频 busy polling。

对应测试应从“整个 stream loop 每 16ms sleep”调整为：

- 输入保持绝对 16ms cadence。
- WebRTC processing 不受 input cadence 限制。
- 输入仍优先于同一时刻的 inbound media burst。

### Phase 2：消除实时队列 bufferbloat

libpeer RTP queue：

- 增加 monotonic arrival timestamp。
- 以毫秒和 packet age 为主要限制。
- 拥塞时优先丢最旧包，不拒绝最新包。
- 按 SSRC/媒体类型区分队列或预算。
- `media_enabled=false` 期间只保留短 startup window。
- 重新启用时清理过龄包并主动请求新 IDR。

MediaPipeline：

- 视频初始限制为 2–4 个 AU，或 30–60ms frame age。
- 超龄时立即清理旧 reference chain，不等待 32MiB/2048 AU 上限。
- 保持最新完整画面，恢复时尽快切到新 IDR。
- 音频改为按时间限制，低延迟模式从 40–80ms 范围测试。
- 网络稳定后自动将自适应音频 buffer 降回低水位。

### Phase 3：deadline-aware NACK 和 frame recovery

为 missing packet 保存：

- first detected time。
- NACK attempts。
- next retry time。
- owning frame deadline。
- repaired state。

局域网模式建议在 deadline 内执行 2–3 次短间隔 NACK，间隔根据持续 RTT 调整。

jitter hold 应改为：

```text
frame deadline = 基础帧预算 + media RTT + 实际 arrival jitter
```

LAN 初始可从 15–35ms 范围测试。超过 deadline 后：

- 放弃不完整 frame。
- 停止等待旧重传。
- 丢弃依赖损坏参考帧的 P-frame。
- 保持上一张有效画面。
- 立即请求 IDR。
- 允许新 IDR 越过陈旧的不完整 frame 恢复。

同时明确 recovery 类型：

- 普通 RTP loss：通常只等待 IDR，不立即重建 decoder/GPU。
- decoder status error：重置 decoder。
- surface generation/分辨率变化：安全退休 GPU 资源并重建。

### Phase 4：持续 RTT 和自适应码率

- 使用 RTCP SR/RR 的 LSR/DLSR，或持续 ICE consent check，获得运行期 RTT。
- RR 填入实际 RTP interarrival jitter。
- 以 loss、RTT、queue age、decode/render load 驱动 receiver bitrate。
- 拥塞时快速下降，稳定时缓慢恢复。
- 保持用户 profile 的最低/最高码率边界。
- 缩短动态 feedback 周期，但避免不必要的 RTCP spam。
- 验证 Xbox 是否实际遵守动态 REMB；若只读取启动 capability，需要设计受控 profile restart。

### Phase 5：decoder、present 和 NvMap 生命周期

- AVPacket 使用 RTP 90kHz 时钟设置 PTS/DTS。
- 使用 decoder 输出 PTS 或 `best_effort_timestamp` 映射回统一纳秒时钟。
- 统计真正 submitted/completed 的 unique displayed frame。
- 真机验证 `extra_hw_frames=16` 的 pipeline 深度。
- 验证 `AV_CODEC_FLAG_LOW_DELAY` 对 Switch NVDEC 的实际效果。
- mapping cache 增加基于 command-ring fence 的安全 eviction。
- surface generation 或分辨率变化后安全清理旧 mapping。
- 普通 frame 和普通 RTP recovery 继续避免全局 `waitIdle()`。

## 7. 验证矩阵

### 7.1 基础场景

- 720p60、1080p60。
- 局域网有线 Xbox + 5GHz/6GHz Switch。
- 每个场景至少 10–30 分钟。
- 多次进入/退出串流。
- 多次 decoder recovery。

### 7.2 网络故障场景

| 类型 | 参数 |
|---|---|
| 随机丢包 | 0.1%、0.5%、1% |
| Burst loss | 连续 2、5、10 包 |
| Jitter | 2ms、5ms、10ms |
| Reorder | 1%、5% |
| 固定 delay | 2ms、5ms、10ms |
| 带宽阶跃 | 高码率突然下降，再恢复 |

### 7.3 重点验收项

- 稳态 RTP 和 AU queue age 接近 0。
- 不出现随运行时间增长的累计延迟。
- 轻微丢包通过 NACK 在 frame deadline 内恢复。
- 不能恢复时快速进入 IDR，而不是冻结 60–180ms 后再决定。
- 网络恢复后不回放陈旧 backlog。
- overlay displayed FPS 与 unique present frame 数一致。
- `missing`、`h264_corrupt`、`srtp_fail`、`decode_errors` 和 `rtp_queue_drop` 可关联到具体 recovery。
- 长时间运行 mapping cache 不达到 32 上限。
- 真机无 GPU queue error、NvMap 生命周期错误或音频 underrun。

## 8. 本轮验证结果

已通过桌面测试：

```text
video_jitter_tests
audio_reorder_tests
rtp_clock_tests
av_sync_tests
audio_timing_tests
```

已通过现有 Python 回归，覆盖：

- Xbox stream session ordering。
- libpeer SCTP、DTLS、media queue。
- jitter、NACK、PLI。
- zero-copy Borealis present 和 frame lifetime。
- NVDEC safety/fallback/status handling。
- bitrate feedback 和性能统计结构。
- audio decoder configuration。

其他检查：

- `git diff --check` 通过。
- 现有 `build/switch/LunarNX.nro` BSS 为 20.4MiB，低于 32MiB 回归线。

需要注意：本轮没有重新在 Docker 中构建当前未提交工作区，因此 20.4MiB 只代表现有构建产物。

## 9. 当前证据边界和残余风险

尚未完成：

- 真实 Xbox RTP 抓包。
- 带 loss/jitter/reorder/bandwidth step 的 mock-stream 测试。
- Ryubing 网络波动 smoke test。
- 真实 Switch 长时间 1080p60 NVDEC/NvMap 测试。
- input-to-photon 测量。

Ryubing 可以验证协议、RTP/SRTP、基础解码和渲染回归，但不能替代真实 Switch 对以下项目的证明：

- hbmenu/NRO loader 兼容性。
- NVDEC surface pipeline 深度。
- NvMap external mapping 生命周期。
- Audren 实际缓冲延迟。
- Wi-Fi 抖动和真实 input-to-photon 延迟。

## 10. 实施约束

- Switch 构建必须使用 Docker/devkitA64，不在 macOS 构建 Switch-targeted libraries。
- 保持 legacy `lib/libpeer` 为 active WebRTC provider。
- legacy libpeer 的可复现修改必须同步到 `tools/libpeer_legacy/legacy-libpeer-switch.patch` 和 README。
- 不删除 `CONFIG_PACKET_BUFFER_SIZE` packet-buffer fix。
- 不增加大型静态网络、解码或 fallback heap，保持 NRO BSS 小于 32MiB。
- 不在应用启动阶段提前创建或启动 streaming controller。
- 所有 Switch/streaming 改动完成后执行 Docker NRO build、BSS test、focused tests 和至少一次 Ryubing mock-stream smoke test。
- 模拟器结果必须与真实 Switch 结果分开报告。

## 11. 推荐实施顺序

```text
Phase 0  测量与故障注入
  → Phase 1  transport 与 16ms input 解耦
  → Phase 2  RTP/AU/audio 队列限龄
  → Phase 3  deadline-aware NACK/IDR recovery
  → Phase 4  持续 RTT 与自适应码率
  → Phase 5  decoder PTS、present 统计和 NvMap eviction
  → Ryubing 回归
  → 真实 Switch 验收
```

第一批实现应优先完成 Phase 0–2。只要 16ms transport polling 和陈旧 FIFO 仍存在，后续对 jitter、码率或 GPU 的优化结果都可能被排队延迟掩盖。

## 12. 后续项：解码帧显示调度（2026-08-26）

本轮真机延迟日志显示，网络良好窗口中的解码帧显示队列等待中位数约为
17ms，已经接近一个完整的 60Hz 刷新周期。该等待主要来自解码完成时刻与
Borealis `View::draw()` 的相位差，不是 NVDEC 吞吐或 Deko3D command submit
本身过慢。

对照实现：

- LunarNX：容量为 2 的 decoded pending 队列；Home/Good 使用
  `RealtimeAdaptive`，仅当积压超过一帧且最老帧超过 25ms 时淘汰旧帧；
  最终由 Borealis draw 驱动 present。
- Green-NX Steady：单帧 latest-frame mailbox；独立 59.94Hz 软件时钟驱动
  自有 deko3d swapchain，默认 newest-wins。Smooth 模式额外保留一个源帧
  以换取更稳定的 cadence。
- Moonlight-Switch：Borealis draw 驱动、默认上限为 3 帧的 decoded FIFO；
  队列为空时重复上一帧，拥塞时才淘汰最老帧，策略更偏平滑而非最低延迟。

后续实现方向已经确认，但本轮暂不修改运行代码：

1. Home/Good 增加真正的 `RealtimeLatest` 单帧 mailbox，不等待 25ms 后才
   合并积压帧。
2. 将 latest-frame 选择尽可能后移到 Borealis 最终 framebuffer submit 前，
   避免普通 View draw 后刚完成的帧额外等待一整个刷新周期。
3. 增加动态 Smooth 模式：稳定 LAN 使用 latest；组帧抖动、重复显示率或
   丢帧升高时短期保留一帧，恢复稳定 2–3 秒后再退出，使用迟滞避免抖动。
4. 保持 Cloud/差链路的顺序缓冲和 H.264 依赖帧顺序解码；禁止在解码前
   任意跳过参考帧。

建议验收目标：

| 模式 | decoded-to-submit p50 | p95 | unique displayed FPS |
|---|---:|---:|---:|
| Home RealtimeLatest | 不高于 9ms | 不高于 16.7ms | 不低于 58 |
| 动态 Smooth | 不高于 25ms | 不高于 33.3ms | 尽量接近 59–60 |

该优化需要保持 Borealis 菜单、GPU mutex、command-ring fence、decoder reset
和 GPU quarantine 的现有生命周期约束。不能直接照搬 Green-NX 在串流期间
完全独占默认窗口的实现，也不能仅通过增大 decoded FIFO 来解决平滑问题。
