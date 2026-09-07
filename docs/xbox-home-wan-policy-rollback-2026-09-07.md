# Xbox Home 异地延迟／卡顿策略回退（2026-09-07）

## 范围和原因

v0.3.1 的 Home 策略把异地主机连接和局域网连接归为同一类。丢包后，超过
3 帧的积压可能使队头等待被 120ms 上限截断，早于异地重传到达；显示端又
从 v0.2.0 的 latest-frame 变为两帧 FIFO，短时间积压时先显示旧帧。

修复只回退 Home 的恢复预算和已解码帧选择，不整体回退应用或 WebRTC：

- Home 普通残帧／已知缺包窗口恢复为 `clamp(2*RTT+20, 60, 180)` ms。
  RTT 未知时沿用 v0.2.0 的 120ms 初值。
- Home 恢复关键帧窗口恢复为 `clamp(RTT+150, 300, 800)` ms。
- Home 超过 8 个组帧条目才使用队头积压规则。阈值为 `max(80, RTT+20)` ms，
  jitter buffer 在使用时再与该帧的实际窗口取小值；不能提前用普通帧的
  180ms 上限截断恢复关键帧的长窗口。
- RTT 使用 raw 和 smoothed 的较大值，保留新版对 RTT 突增的及时响应。
- Home 初始化、正常运行和恢复期间都选 `RealtimeLatest`：在现有 present
  边界淘汰更旧的已解码帧，保留最新帧。编码参考帧仍按顺序解码。
- xCloud 的自适应策略、PS 的 FIFO、音频缓冲、码率自适应、输入、ICE、
  GPU fence／资源生命周期不作策略调整。不改 legacy libpeer checkout。

## 取舍和影响

局域网与异地的 Xbox 主机串流都会应用该回退。完整且不被旧残帧挡住的视频
立即交付，不按上述窗口预缓冲。缺包无法修复时则可能比 v0.3.1 多等一段
时间，以换取更高的重传成功率；普通帧依然有 180ms 窗口上限和既有内存／
总帧龄保护。高 RTT 超出普通帧预算的网络仍可能需要丢帧、等待关键帧恢复。

显示端优先最新画面，会跳过已经落后的已解码帧。帧交付很不均匀时，显示
节奏可能没有 FIFO 平滑，但不会为了逐帧播放旧画面再增加一层排队。

这不是“所有场景固定快一帧”的保证，也不能替代真机 input-to-photon 测量。

## 回归覆盖

- `video_rtp_jitter_buffer_test.cpp` 使用实际 `computeVideoJitterPolicy` 和
  `VideoRtpJitterBuffer`，覆盖 8 个 RTT／额外重传延迟组合 × 3 个质量等级。
  60ms RTT + 30ms 额外延迟、140ms RTT 等场景修复前会丢弃可恢复的帧。
- 验证未知 RTT、LAN、WAN 下完整帧立即输出，以及缺包永不到达时能有界退出。
- 验证 300ms RTT 下恢复 IDR 的重传不会被普通帧窗口或后续积压提前截断。
- `realtime_latency_policy_test.cpp` 验证 Home 在 Good/Poor/Fair 和 recovery
  状态下，即便旧帧只等了 1ms，也会从两帧中选最新帧；继续保留 Cloud/FIFO
  和 25ms RealtimeAdaptive 的原有断言。
- 更新旧的 Home 参数断言，以记录此次有意回退的策略。

## 验证结果

- Docker `devkitpro/devkita64:20251117`、legacy libpeer、moonlight curl 的完整
  Switch NRO 构建通过；BSS 为 20.4 MiB，低于 32 MiB 回归线。
- 本地 Chiaki 预编译 SDK 起初缺少当前源码要求的 route-preference 接口。
  备份原 SDK 后，使用现有 `tools/chiaki_switch/build_in_docker.sh` 重建；
  没有改 Chiaki 源码 checkout 或补丁，重建后的 Switch ABI 检查通过。
- `video_jitter_tests`、`realtime_latency_policy_tests`、`adaptive_bitrate_tests`
  全部通过，包括新增的 WAN 重传、关键帧长窗口和 latest-frame 测试。
- 重新运行最初的 A/B 驱动：28 组丢包、28 组无损、84 组质量输入的全部
  CSV 字段均与 v0.2.0 一致。这些是确定性测试，不是真机延迟或发生率。
- session order、DTLS read loop、datachannel PPID、media pipeline scheduling、
  zero-copy frame lifetime、renderer hardware safety、realtime stream safety、
  libpeer media queue、video pipeline logging 的 Python 检查通过。
- `libpeer_sctp_config_test.py` 有一项既有失败：它查找固定的
  `while (packets_processed < PEER_CONNECTION_MAX_PACKETS_PER_LOOP)`，但当前
  忽略的本地 libpeer 已通过 `pc->config.max_packets_per_loop` 配置该预算。
  用未修改的 HEAD 文件配合同一本地依赖复核，得到相同失败；此次未放宽
  测试或改动 libpeer 来掩盖它。
- `git diff --check` 通过。
- 按用户要求取消模拟器测试；只验证了测试 NRO 启动到平台页，没有完成
  mock 串流 smoke。独立测试实例与 mock 服务已停止，没有执行真机实测。

当前 NRO 是现有 v0.3.2 开发树上的测试构建，未发布新版本。仍需真实 Switch
在相同异地网络、分辨率和码率下做实际延迟／平滑度对照。

