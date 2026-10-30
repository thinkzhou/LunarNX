# libdatachannel / libjuice 性能与 Switch 可移植性评估

日期：2026-10-30。正式应用继续使用 legacy libpeer；本研究没有修改运行时、认证、串流控制或用户已有的工作区改动。

## 目前结论

在本次桌面合成 SRTP 基线与持续负载测试中，libdatachannel 的送达回调延迟较低，但接收进程 CPU 开销较高。现有证据不支持“换库即可全面提高性能”，也不能据此推断 Switch 真机效果。

## 桌面测试方法

- Apple M2，8 个逻辑核，macOS 26.5.2 / arm64，Apple clang 21.0.0，Release 构建；两个独立进程通过同一台机器的 en0 IPv4 地址通信，不经过物理 Wi-Fi 链路。
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

## 轮询间隔敏感性

将 harness 的请求 idle sleep 从 1000 微秒改为 100 微秒，15 / 50 Mbps 各做 2 次、每次 5 秒的测量。sleep_for 的实际唤醒时间受 macOS 调度影响，不能等同于精确的 100 微秒周期。

| 目标码率 | libpeer CPU | libdatachannel CPU | libpeer P95 | libdatachannel P95 |
|---|---:|---:|---:|---:|
| 15 Mbps | 9.00% | 8.61% | 1.474 ms | 0.113 ms |
| 50 Mbps | 15.74% | 16.03% | 4.106 ms | 2.051 ms |

8/8 样本完成且没有丢包。15 Mbps 下 libpeer P95 比基线下降，但 CPU 上升。50 Mbps 下 libdatachannel 两次 P95 分别为 0.117 / 3.984 ms，其中后一次 P99 为 49.439 ms；不能隐藏这一尾延迟异常。两个小样本和非隔离桌面环境不足以给出稳定性保证。该实验说明必须把 owner-loop 调度纳入 A/B，不能把整段差异全部归因于库。

原始数据：`build/webrtc_bench/poll100us/results.json`。

## 持续负载复测

50 Mbps、每次预热 1 秒后测量 30 秒，每个后端共 3 次；后两轮包含反向执行顺序。每次收到 156,250 个测量包，覆盖多次 16 位 RTP 序号回绕。

| 后端 | CPU 中位数 | P95 中位数 | P95 范围 | P99 范围 |
|---|---:|---:|---:|---:|
| libpeer | 12.78% | 5.127 ms | 4.608–50.276 ms | 5.977–170.802 ms |
| datachannel | 18.68% | 0.617 ms | 0.547–0.644 ms | 1.783–2.958 ms |

6/6 次完成且没有丢包、重复包、无效包或失败发送。libpeer 第一次长测存在明显尾延迟异常（P99 170.802 ms），随后两次为 5.977 / 6.471 ms。没有额外插桩来定位这个异常，因此不能断言是库内部、发送排队还是主机调度造成；全部样本均保留。

基线、敏感性和持续负载合计 32 次正式测量，均完成。它们支持“本机此配置下，libdatachannel 有降低传输回调延迟的潜力”，但不支持“真机整体性能必然更好”。

原始数据：`build/webrtc_bench/sustained/results.json` 与 `sustained-repeat/results.json`。

```sh
python3 tools/webrtc_bench/run_trials.py --rates 15 50 --repeats 2 --seconds 5 --idle-us 100 --output build/webrtc_bench/new-poll100us
python3 tools/webrtc_bench/run_trials.py --rates 50 --repeats 3 --seconds 30 --output build/webrtc_bench/new-sustained
```

## Switch 编译与对方适配

下载 YOUYOU 公开源码快照，在生成目录内运行其依赖下载脚本，校验所有锁定归档与 31 个源文件覆盖的哈希，再应用适配。没有把对方应用代码或依赖改动拷入 LunarNX 正式路径。

- YOUYOU 源码归档 SHA-256：`d7e2c77bbe072e3d5236e205fa6bfb01d9ce1dcc7761f5b49fb479514f84ddb2`。
- 对方 libdatachannel：`9e6a13abbb6846c003d817d0387b6706466e2b03`；libjuice：`b509b6c1f90e4a1d4753e6bde62ffbeb4d10c6bb`；mbedTLS：`v3.6.4`。注意与桌面 A/B 的 libdatachannel 提交不同。
- 指定 Docker 镜像 `devkitpro/devkita64:20251117`；本次镜像 ID：`sha256:322d774b79dddc6b062422686e0127fd571e041afb3b8fc935204432e7483af6`。
- 使用对方 `cmake/switch.cmake`，`UU_RTC_TESTS=ON`、`NO_MEDIA=OFF`、`NO_WEBSOCKET=ON`，实际成功构建 `datachannel-static`，包含 mbedTLS、libjuice、usrsctp、libsrtp 编译。
- 这是静态库交叉编译成功；没有完成最终应用链接、生成/加载新 NRO、真实 ICE/DTLS/SCTP 运行或 Xbox 协议互通。静态归档允许未解析符号，因此编译成功不能替代运行验证。

值得研究的适配：

1. libjuice 用 Switch 地址获取路径替代 getifaddrs，处理非阻塞 socket、接收缓冲与 poll 唤醒；有独立的批量接收实验开关，未因此宣称已改善性能。
2. usrsctp 保持 Horizon 带长度的 sockaddr_conn 布局，并使库内部与公开头保持一致。
3. mbedTLS 使用 randomGet 提供熵与 Switch 单调时钟钩子；应用提供实现，不能只靠禁用默认平台模块。
4. C++ 随机数状态改走 pthread TLS，清理任务改用可管理后台工作，避免依赖不适配的线程行为。
5. SRTP 配置支持 ARMv8 mbedTLS 加密路径；桌面 A/B 没有测这条 Switch 专用加速路径。

复现本次静态库构建（先在 `build/webrtc_bench/YOUYOU-Remote-ns-main` 放置相同源码快照）：

```sh
python3 build/webrtc_bench/YOUYOU-Remote-ns-main/scripts/fetch_deps.py
python3 build/webrtc_bench/YOUYOU-Remote-ns-main/scripts/patch_deps.py
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    export DEVKITPRO=/opt/devkitpro
    cmake -S build/webrtc_bench/YOUYOU-Remote-ns-main \
      -B build/webrtc_bench/donor-switch \
      -DCMAKE_TOOLCHAIN_FILE=/work/build/webrtc_bench/YOUYOU-Remote-ns-main/cmake/switch.cmake \
      -DCMAKE_BUILD_TYPE=Release -DUU_RTC_TESTS=ON \
      -DNO_MEDIA=OFF -DNO_WEBSOCKET=ON
    cmake --build build/webrtc_bench/donor-switch --target datachannel-static -j6
  '
```

构建日志在 `build/webrtc_bench/donor-switch-config.log` 与 `donor-switch-build.log`。

## 已知边界与建议

- 本测试不覆盖真实 Xbox/xCloud 的 SDP、数据通道初始化、输入、反馈、TURN、WAN 打洞或 PS 协议。libjuice 的广域网连接成功率没有测量。
- 早期 harness 探索中，libpeer 作为 offerer 与该发送端发生握手超时，也有开启诊断后成功的样本；原因尚未定位。正式矩阵统一改为接收端 answerer。不可将这些早期失败当作吞吐结果，也不可声称已验证 LunarNX 现有 offerer 业务路径。
- 没有真实 H.264 内容、解码显示或控制回路；合成包验证不了画质、花屏恢复或端到端操作延迟。
- 双方在指定码率均可收齐，只能说明本机该负载可承受，未测最大吞吐容量。桌面后台负载、功耗状态和调度未隔离。
- 没有执行新后端的 Ryubing mock-stream 或真实 Switch A/B：当前交付是桌面独立 harness 和对方静态库编译实验，尚无可运行的新 LunarNX Switch 后端。没有修改生产 Switch 代码或构建入口，因此未重建 LunarNX NRO。

建议继续保留 libpeer。下一步有价值的验证是独立 Switch 传输探针，在同一真机、同一发送端上比较两套库，并加入音频与高频输入数据通道；同时检查 SRTP 加速、内存、线程数、BSS 和退出清理。在此之前，不建议整体替换现有 Xbox 传输层。

参考：[YOUYOU 依赖适配清单](https://github.com/doke69/YOUYOU-Remote-ns/blob/main/scripts/dependency-overrides/manifest.json)、[Switch 平台熵与时间钩子](https://github.com/doke69/YOUYOU-Remote-ns/blob/main/source/platform_switch.cpp)、[libdatachannel 官方源码](https://github.com/paullouisageneau/libdatachannel)。

## 验证与残留问题

- native harness 的 Release 构建、Python 编译、shell 语法与 `git diff --check` 通过。单独开启日志的 smoke 确认两种接收端实际协商结果均为 `SRTP_AES128_CM_SHA1_80`；日志在 `build/webrtc_bench/profile-verification/*/sender.log`。
- `xbox_stream_session_order_test.py`、`libpeer_dtls_read_loop_test.py`、`datachannel_ppid_test.py` 通过。
- `libpeer_sctp_config_test.py` 失败：它检查接收循环的固定文本形式，与当前忽略目录里 libpeer 的可配置循环形式不匹配。本研究没有修改该测试或 libpeer；这是现有工作区的静态检查失败，不能写成全部回归通过。
- 对已有的 `build/switch/LunarNX.nro` 执行 BSS 检查通过（20.4 MiB）。该文件是此前产物，不代表本次生成或验证了新后端 NRO。
- 原始回归结果在 `build/webrtc_bench/regression-results.json`。正式测量期间未运行这些回归脚本；最后重新构建及 profile 验证属于单独 smoke，不混入性能矩阵。
- 这些库对应 Xbox/xCloud 的 WebRTC 路线；PS 的 Chiaki 传输不会因替换它们自动受益。
