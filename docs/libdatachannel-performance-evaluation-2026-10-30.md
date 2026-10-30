# libdatachannel / libjuice 性能与 Switch 可移植性评估

日期：2026-10-30。正式应用继续使用 legacy libpeer；本研究没有修改运行时、认证、串流控制或用户已有的工作区改动。

## 目前结论

在本机合成 SRTP 测试中，libdatachannel 的送达回调延迟较低，但接收进程 CPU 开销较高。现有证据不支持“换库即可全面提高性能”，也不能据此推断 Switch 真机效果。

## 桌面测试方法

- macOS 26.5.2 / arm64，Release 构建；两个独立进程通过同一台机器的 en0 IPv4 地址通信，不经过物理 Wi-Fi 链路。
- 两个接收端使用同一 libdatachannel 发送端；1200 字节 RTP 包，60 Hz 突发发送，实际经过 ICE、DTLS 和 SRTP。包中含发送时间与独立的 64 位编号；它不是可解码的视频。
- 双方均作为 SDP answerer；没有启用 SCTP、音频、编解码、显示、NACK/RTX 或应用层抖动缓冲。libpeer 使用 raw RTP 回调；libdatachannel 使用 Track 原始 RTP 回调。
- 双方及发送端链接同一份现有 native libSRTP 静态库，使用 SRTP_AES128_CM_SHA1_80。避免将 libdatachannel 默认的 OpenSSL AES-GCM 与 libpeer 的软件 AES-CM 混为网络库优势。
- DTLS 仍分别采用 mbedTLS 与 OpenSSL；建连指标包含这个差异。桌面 CPU 数据也不是对方 Switch 的 ARMv8 mbedTLS 配置。
- 预热 1 秒，测量 6 秒，每个码率重复 3 次，第二轮交换执行顺序。主循环 idle sleep 为 1000 微秒；libdatachannel 自己的网络工作线程按事件调度。
- CPU 为接收进程全部线程的 user+system 时间 / 测量墙钟时间，100% 表示一个逻辑核；不包含独立发送进程。延迟从发送端 Track::send 前到接收回调入口，包含发送排队和 OS 调度，不是显示或操作延迟。

## 基线结果

下表为三个独立样本的中位数；P95 列为各样本 P95 的中位数，非合并分布的 P95。

| 目标码率 | libpeer CPU | libdatachannel CPU | libpeer P95 | libdatachannel P95 | 双方丢包 |
|---|---:|---:|---:|---:|---:|
| 15 Mbps | 6.24% | 8.95% | 2.051 ms | 0.582 ms | 0 / 0 |
| 30 Mbps | 9.84% | 13.90% | 2.865 ms | 0.521 ms | 0 / 0 |
| 50 Mbps | 15.04% | 18.86% | 4.834 ms | 0.660 ms | 0 / 0 |

18/18 正式基线样本完成，合计发送并收到 356,250 个测量包，无无效包、重复包或失败发送。实际发送速率范围为 14.991–49.998 Mbps（跨三个目标档位）。

## 源码与复现

- libdatachannel：`188ec93f6a0b01c2032ddb4c8c013118f7f30232`。
- legacy libpeer 基础提交：`9319aa434cb9e893faed0293ba9d2a21eca59c8b`；本地已有 LunarNX 补丁，非干净上游。
- libpeer 源码组合指纹：`e476610876345371ca730fd1668b6705e4bf074042bdec099819cf68ceceae87`。
- 共享 native libSRTP SHA-256：`150bc21ecef7c910bc1659180f75a195274ec9146949cd25443a6a9e91798e8c`。

```sh
bash tools/webrtc_bench/build_native.sh
python3 tools/webrtc_bench/run_trials.py --rates 15 30 50 --repeats 3 --seconds 6 --output build/webrtc_bench/new-baseline
```

原始数据：`build/webrtc_bench/baseline/results.json`（生成物，不提交）；SHA-256：`b1a4a2430068bd1b79f453cf59b783a7011e56dd4408d35297e0eb3112aad792`。每个子目录包含 SDP、双方日志及单次结果。测试脚本和更多限制见 `tools/webrtc_bench/README.md`。
