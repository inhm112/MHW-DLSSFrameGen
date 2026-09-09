# MHWFG 首个公开测试版

为《怪物猎人：世界／冰原》提供 DLSS 帧生成和固定倍数 MFG，配合 MHWSS 使用。

## 功能

- MHWFG 首页：Active 开关、2–6x 请求倍数、限帧与配置保存。
- Advanced 完整设置、界面缩放和可拖拽调整大小的窗口。
- 游戏交换链、资源状态、Reflex 与菜单同步适配。

## 安装

下载 `MHWFG-runtime.zip`，将其中的 `d3d12.dll`、`OptiScaler.ini` 和 `OptiScaler` 文件夹复制到游戏根目录，使用 `MHWSSLauncher.exe` 启动。按 Ins 打开菜单，选择 2x 并勾选 Active。

覆盖文件前请备份。详细安装与使用说明见仓库 README。

## 本轮验收

已完成两种玩家手动安装流程：MHWSS + MHWFG，以及 MHWSS + 标准 40MFG + MHWFG。用户反馈启动、2x、第二种场景的 3x、Active 切换和流畅度正常。

运行组件：DLSS SR 310.5.0、DLSSG 310.7.0、Streamline 2.12.0。

## 已知问题

冰原部分冰崖和坡面在开启 Active 后可能出现白色或橙色闪烁，遇到时可暂时关闭 Active。

RTX 40 系使用 MFG，请前往 [dashdogy 的 RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock)，按作者提供的正常安装步骤安装，即可在本模组中开启 MFG。
