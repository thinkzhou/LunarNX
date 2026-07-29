# LunarNX

[English](README.md) | 简体中文

面向 Nintendo Switch 自制软件环境的非官方 Xbox Remote Play 与 Xbox Cloud Gaming 客户端。

![LunarNX 在 Nintendo Switch 上进行 1080p Xbox 游戏串流](docs/screenshots/streaming.jpg)

## 项目亮点

- 在同一局域网内串流自己的 Xbox 主机
- 浏览并游玩支持的 Xbox Cloud Gaming 游戏
- 提供 720p、1080p 和 1080p HQ 三种画质配置
- 硬件视频解码、低延迟音频、手柄输入和震动反馈
- Xbox 主机发现与唤醒、云游戏库、最近游玩和搜索
- 可选的画面放大与锐化
- 遇到短时丢包或网络波动时能够快速恢复画面
- 英文、简体中文和繁体中文界面

当前版本已在真实 Switch 硬件上测试本地主机串流与 Xbox Cloud Gaming，并覆盖全部三种画质配置。实际体验仍会受到网络、Xbox 服务、账号、地区和具体游戏的影响。

> [!WARNING]
> LunarNX 仍处于早期开发阶段。它需要能够运行 NRO 的改装 Nintendo Switch，无法在未经修改的零售版主机上运行。

## 支持情况

| 功能 | 状态 |
| --- | --- |
| 同一局域网内的 Xbox Remote Play | 已在真实 Switch 硬件上测试 |
| Xbox Cloud Gaming | 已在真实 Switch 硬件上测试 |
| 720p / 1080p / 1080p HQ | 已完成本地与云游戏串流测试 |
| 通过互联网连接自己的 Xbox 主机 | 实验性；取决于网络能否建立直连 |

异地连接自己的 Xbox 主机无法保证适用于所有网络，因为 LunarNX 目前没有 TURN 中继回退能力。推荐使用同一局域网内的 Remote Play 或 Xbox Cloud Gaming。

## 使用要求

- 能够运行 Homebrew NRO 的 Nintendo Switch，通常需要 Atmosphere CFW
- 以 Title Override / 完整内存模式运行 Homebrew Menu
- 已启用 Remote Play 的 Xbox One 或 Xbox Series 主机，或者具备 Xbox Cloud Gaming 权限的账号
- 稳定的 5 GHz Wi-Fi 或有线网络

Xbox Cloud Gaming 的可用性取决于账号权限和地区。通常需要 Game Pass Ultimate，部分符合条件的免费游戏可能有所不同。

## 安装

1. 从 [Releases 页面](https://github.com/thinkzhou/LunarNX/releases) 下载 `LunarNX.nro`。
2. 将文件复制到 SD 卡：

   ```text
   sdmc:/switch/LunarNX/LunarNX.nro
   ```

3. 使用 Title Override 打开 Homebrew Menu，然后启动 LunarNX。

请勿分享 `sdmc:/switch/LunarNX/` 目录中的内容。该目录可能包含 Microsoft/Xbox 登录数据和诊断日志。

## 开始串流

1. 选择 **开始登录**。
2. 在手机或电脑上打开界面显示的 Microsoft 设备登录地址并输入验证码。
3. 选择账号中的 Xbox 主机，或者打开 Xbox Cloud Gaming 游戏库。
4. 在设置中选择 720p、1080p 或 1080p HQ。
5. 选择 **开始游戏** 或 **唤醒并连接**。

如果不确定网络质量，建议先使用 720p，再尝试 1080p 或 1080p HQ。首次建立连接可能需要一分钟；主机处于休眠状态时，可能需要再次尝试唤醒。

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
| L + R + Plus | Xbox Guide / Nexus 键 |
| 左摇杆按下 | L3 |
| 右摇杆按下 | R3 |
| 十字键 / 摇杆 | 十字键 / 摇杆 |

Switch 的 ZL/ZR 是数字按键，因此 Xbox 扳机输入只有 0% 或 100%。

- 从触摸屏右侧边缘向内滑动可打开串流菜单，其中包含性能信息、Xbox 按钮和断开串流操作。
- 同时按下 **L + R + Plus** 可打开 Xbox 导航菜单。
- 使用按键停止串流时，请在三秒内连续两次同时按下 **Minus + Plus**。单独按下 Minus 或 Plus 仍会发送 Xbox View 或 Menu。

## 常见问题

- **应用无法启动：** 请使用 Title Override 打开 Homebrew Menu。Applet Mode 的可用内存可能不足以支持串流和硬件解码。
- **画面卡顿或出现马赛克：** 使用 5 GHz Wi-Fi、靠近无线路由器、减少其他网络流量，或者改用 720p。
- **找不到自己的 Xbox：** 确认主机已经开启 Remote Play，并且 Xbox 与 Switch 位于同一局域网。可以先尝试唤醒主机，再刷新列表。
- **异地连接自己的 Xbox 失败：** 当前连接必须在网络之间找到直连路径，严格 NAT 或防火墙可能阻止连接。
- **无法使用云游戏：** 确认账号与所在地区具备 Xbox Cloud Gaming 权限。

## 更多实机截图

| 查找 Xbox 主机 | 串流设置 |
| --- | --- |
| ![LunarNX 查找 Xbox 主机界面](docs/screenshots/find_xbox.jpg) | ![LunarNX 分辨率与解码器设置](docs/screenshots/settings.jpg) |

![LunarNX 正在建立 Xbox Remote Play 会话](docs/screenshots/connecting.jpg)

## 问题反馈与隐私

反馈问题前请阅读 [SECURITY.md](SECURITY.md)。请勿公开 Token 文件、完整日志、原始 Xbox 响应、主机标识符或公网 IP。有效的问题报告应包含所选画质、Switch 型号和系统版本、本地或云游戏串流、网络类型，以及能够说明问题的最小化脱敏日志片段。

## 开发者文档

构建步骤、验证命令、架构说明和模拟器测试请查看[开发指南](docs/development.md)。欢迎参与贡献，提交代码前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 致谢与许可证

LunarNX 使用或参考了 [libpeer](https://github.com/sepfy/libpeer)、[Borealis](https://github.com/XITRIX/borealis)、[FFmpeg](https://github.com/FFmpeg/FFmpeg)、[libnx](https://github.com/switchbrew/libnx)、[deko3d](https://github.com/devkitPro/deko3d)、[XStreaming](https://github.com/Geocld/XStreaming)、[Greenlight](https://github.com/unknownskl/greenlight)、[Moonlight-Switch](https://github.com/XITRIX/Moonlight-Switch)、[wiliwili](https://github.com/xfangfang/wiliwili)、[libnxbox](https://github.com/ursusworks/libnxbox) 和 [xbox-xcloud-player](https://github.com/unknownskl/xbox-xcloud-player)。

LunarNX 自有源码使用 [MIT License](LICENSE)。第三方源码、库、补丁和最终链接产物仍受各自许可证约束，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

LunarNX 与 Microsoft、Xbox、Nintendo 以及文中提到的开源项目维护者不存在隶属、授权或背书关系。所有产品名称和商标均归其各自所有者所有。
