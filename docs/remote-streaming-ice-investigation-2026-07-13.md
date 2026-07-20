# 异地 Xbox 串流 ICE 调查报告

- 日期：2026-07-13
- 范围：LunarNX（macOS `sdp_probe` / libpeer）与 XStreaming-desktop / XStreaming 手机版对照
- 目标：解释“内网串流正常，异地串流失败”的根因，并记录可用网络下的真实连通路径

## 1. 结论摘要

**是的：在手机流量 / 手机热点网络下，异地串流走通的主路径是 IPv6 host ↔ host 直连，不是公网 IPv4，也不是 TURN。**

更完整地说：

1. 公司网下，手机 XStreaming、Mac 上的 LunarNX、Mac 上的 XStreaming-desktop 都失败。
2. 运营商流量下，手机 XStreaming 成功。
3. Mac 切到手机热点后：
   - LunarNX `sdp_probe` 成功
   - XStreaming-desktop 成功并出画
4. 两边成功时的 selected pair 都是：

```text
local  IPv6 host  (运营商热点分配地址，已匿名)
remote IPv6 host  (Xbox 原生 IPv6，已匿名，端口 9002)
```

因此：

- 不是“LunarNX 完全不会异地串流”
- 不是“只能靠 TURN”
- 当前这台 Xbox + 该运营商网络的有效路径，优先是 **Xbox 原生 IPv6**
- 公司网失败，主因是出口网络/NAT/UDP/IPv6 路径不可用

## 2. 环境与对照矩阵

| 客户端 | 网络 | 结果 | 观察 |
|--------|------|------|------|
| LunarNX `sdp_probe` | 公司网 | 失败 | 有 IPv4 srflx，ICE 0 回包 |
| XStreaming-desktop | 公司网 | 失败 | 有 srflx + 默认可选 TURN，仍 failed |
| XStreaming 手机 | 公司网 | 失败 | 用户确认 |
| XStreaming 手机 | 运营商流量 | 成功 | 用户确认 |
| LunarNX `sdp_probe` | Mac + 手机热点 | **成功** | ICE connected/completed，DataChannel ready |
| XStreaming-desktop | Mac + 手机热点 | **成功** | 有画面；selected pair 为 IPv6 |

## 3. 协议侧已确认的事实

### 3.1 Xbox 远端 ICE 形态

Xbox 返回的 remote candidates 典型包含：

```text
192.168.1.11:9002                 # 局域网 host 示例，远程无用
2001::... / 2001:0:...            # Teredo IPv6
2001:db8:1::11:9002               # Xbox 原生 IPv6 host 匿名示例
203.0.113.24:9002                 # Xbox 公网 IPv4 匿名示例
203.0.113.24:3074                 # Teredo 解出的 mapped port
```

### 3.2 Teredo 展开

XStreaming / Greenlight / LunarNX 都会把 `2001:` Teredo 解成：

```text
公网IPv4:9002
公网IPv4:mappedPort
```

LunarNX 实现位置：

- `src/app/ice_candidate_processor.cpp`
  - `decodeTeredoAddress`
  - `expandTeredoCandidatesLikeXStreaming`
  - `rewriteCandidatesLikeXStreaming`

实测样例：

```text
2001:0000:4136:e378:8000:f3fd:34ff:8ee7
  -> 203.0.113.24:9002
  -> 203.0.113.24:3074
```

说明：**LunarNX 并不是“没处理远程 candidate”**。

### 3.3 本地 candidate 形态

#### 公司网（失败）

LunarNX 本地大致为：

```text
host  192.168.x.x
srflx 198.51.100.20:xxxxx
```

几乎没有可用的公网 IPv6 直连路径。

#### 手机热点（成功）

LunarNX 本地：

```text
host  172.20.10.8                 # iPhone 热点 IPv4 示例
host  2001:db8:2::75              # 热点 IPv6 匿名示例
srflx 198.51.100.21:xxxxx         # 公网 IPv4 匿名示例
```

XStreaming-desktop 本地还额外有多张虚拟网卡地址，但最终没有选它们。

## 4. 成功路径细节

### 4.1 LunarNX（热点）

结果：

```text
peer connected=true
datachannel=true
ICE: checking -> connected -> completed
```

成功 pair：

```text
local : [2001:db8:2::75]:59571
remote: [2001:db8:1::11]:9002
```

日志：

- `/tmp/lunarnx_hotspot_ice_probe.log`
- `/tmp/lunarnx_hotspot_ice_debug.log`

### 4.2 XStreaming-desktop（热点）

结果：

```text
ice=connected
conn=connected
```

selected pair：

```text
local : [2001:db8:2::75]:53950 typ host UDP
remote: [2001:db8:1::11]:9002 typ host UDP
rtt   : ~0.026s
bytes : sent ~109KB, received ~2.8MB
```

完整 capture：

- `/tmp/xstreaming_hotspot_capture.json`
- `/tmp/xstreaming_hotspot_dev.log`

说明：

- 有画面
- 最终路径与 LunarNX 相同：**IPv6 host 直连**
- 本次 stream URL 未携带 `server_url=turn:...`，即未依赖默认 TURN

## 5. TURN 相关结论

### 5.1 第三方 TURN 源

XStreaming 可以从第三方配置 endpoint 动态获取 TURN 服务。这份报告不保存或
背书共享 TURN 凭据；应由用户提供自己的可授权 TURN 服务。

### 5.2 Desktop 与手机默认策略不同

| 平台 | 默认是否使用该 TURN |
|------|----------------------|
| XStreaming-desktop | **是**（`getServer()` 成功后作为 default server 注入） |
| XStreaming 手机 | **否**（默认 `use_inner_turn_server=false`，需用户开启或手填） |

### 5.3 TURN 本身可用性

从 Mac 直连测试：

- STUN Binding OK
- Allocate + 凭据 OK
- relay 地址可分配（原始地址已匿名）
- 服务端为 Coturn 4.6.1

但：

- 公司网下 XStreaming-desktop 即使拿到 relay candidate，串流仍 failed
- 热点成功路径并未依赖 TURN

所以：

> TURN 可分配 ≠ 异地串流一定成功  
> 当前成功案例也 ≠ 走了 TURN

## 6. 失败路径分析

### 6.1 公司网

共同特征：

1. 能完成 Xbox 信令（session / sdp / ice exchange）
2. 能看到 Xbox 公网 IPv4 / Teredo / LAN candidates
3. ICE connectivity check 无法形成有效 pair
4. LunarNX 侧表现为长时间 checking，最终 DataChannel 超时
5. XStreaming-desktop 侧最终 `connectionstate=failed`

LunarNX 公司网日志中：

- 大量 conncheck 发往局域网候选和公网 IPv4 匿名候选 `203.0.113.24:9002/3074`
- **0 个 STUN response**

### 6.2 为什么容易误判成“只剩 IPv6”

因为：

- 成功时 selected 确实是 IPv6
- 但失败时并不是“代码只会 IPv6”
- 而是 **当前公司网下 IPv4 公网路径也打不通，IPv6 也没有可用路由/回程**

热点成功证明：

- 协议栈能处理 IPv6
- 也能收集 IPv4 srflx
- 只是在好网络里 ICE 最终选了 IPv6

## 7. 对 LunarNX 开发的含义

### 7.1 已成立

1. 信令、SDP、ICE exchange、Teredo 展开、DataChannel 建立，在可用网络下能跑通
2. 异地串流在“手机热点/运营商流量”下可行
3. 当前最有效路径是 Xbox 原生 IPv6 host

### 7.2 仍建议改进

1. **远程场景 candidate 排序**
   - 后置 RFC1918（如 `192.168.1.11`）
   - 提升公网 IPv4 / 原生 IPv6 优先级
   - 减少先打局域网候选浪费的 checking 时间

2. **IPv6 能力要当一等公民**
   - Switch 真机若无公网 IPv6，可能无法复现这次成功路径
   - 需要单独验证“纯 IPv4 异地”是否仍可通

3. **可选 TURN**
   - 可对齐 XStreaming-desktop 的 `server.json` / 自定义 TURN
   - 作为受限 NAT 的兜底，不应当成唯一方案

4. **网络验收环境**
   - 公司网不适合作为异地串流通过/失败的最终判据
   - 应用：
     - 手机热点 / 家宽 / 运营商网络做协议验收
     - 真机 Switch 做最终兼容验收
     - 模拟器公网 NAT 仍可能额外失真

### 7.3 Switch 特别注意

本次成功依赖本机拥有：

```text
2001:db8:2::/48 格式的匿名公网 IPv6 示例
```

若 Switch 系统/网络栈拿不到等价 IPv6：

- 即使协议正确，也可能无法走这次验证过的成功路径
- 需要继续打通：
  - IPv4 srflx ↔ Xbox 公网 IPv4
  - 或 TURN relay

## 8. 关键原始产物

| 文件 | 内容 |
|------|------|
| `/tmp/lunarnx_wan_ice_probe.log` | 公司网 LunarNX 失败总览 |
| `/tmp/lunarnx_wan_ice_debug.log` | 公司网 LunarNX ICE debug |
| `/tmp/lunarnx_hotspot_ice_probe.log` | 热点 LunarNX 成功总览 |
| `/tmp/lunarnx_hotspot_ice_debug.log` | 热点 LunarNX ICE debug |
| `/tmp/xstreaming_hotspot_capture.json` | 热点 XStreaming-desktop ICE/状态 capture |
| `/tmp/xstreaming_hotspot_dev.log` | 热点 XStreaming-desktop 进程日志 |
| `/tmp/xstreaming_ice_dump.json` | 公司网 XStreaming-desktop 失败 dump |

代码参考：

- LunarNX Teredo/ICE：`src/app/ice_candidate_processor.cpp`
- LunarNX session 顺序：`src/app/xbox_stream_session.cpp`
- XStreaming-desktop 默认 TURN：`renderer/lib/get-server.ts` + `renderer/pages/[locale]/home.tsx`
- XStreaming 手机 TURN 默认关闭：`src/store/settingStore.ts` + `src/webrtc/index.ts`
- TURN 配置：XStreaming 的动态第三方配置路径（凭据未记录）

## 9. 一句话结论

**异地串流在可用运营商网络下已经证明可以走通，而且 XStreaming-desktop 与 LunarNX 成功时都走的是 Xbox 原生 IPv6 直连；公司网失败是网络路径问题，不是“完全没实现远程 ICE”。后续开发应把 IPv6 成功路径、IPv4 兜底、TURN 兜底、以及 candidate 排序分开验证。**

## 10. Cleanup note (same day)

After the remote-path experiments, the working tree was cleaned for LAN-first Switch use:

- Keep Teredo expansion and ICE diagnostics helpers.
- Candidate ranking restored to **private LAN IPv4 first**, then public IPv4, then IPv6.
- Desktop `AGENT_ICE_DEBUG` defaults to off; enable with `AGENT_ICE_DEBUG=1` when needed.
- Temporary IPv4-only probe binary and local `lunarnx.log` were removed.
- XStreaming-desktop local config changes used during TURN forcing were restored.

Current product focus: reliable same-LAN Switch streaming.
