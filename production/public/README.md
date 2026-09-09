# MHWFG

**简体中文** | [English](README.en.md)

> 本模组不提供 MHWSS 的任何文件，请使用者自行前往 [原作者的 Patreon 页面](https://www.patreon.com/huutaiii/posts/mhwss-v1-0-2-mod-142322059)获取 MHWSS 并支持原作者。

为《怪物猎人：世界／冰原》提供 **DLSS 帧生成（FG）与多帧生成（MFG）**，配合 MHWSS 使用，基于修改版 [OptiScaler](https://github.com/optiscaler/OptiScaler)。

## 功能

- 帧生成开关与 **2–6x** 固定倍数选择，可用倍数随显卡和运行组件能力变化。
- 简洁的 MHWFG 首页，提供限帧、配置保存和界面缩放。
- **Advanced** 保留完整 OptiScaler 设置。
- 窗口支持拖动位置、拖拽右下角调整大小。

## 安装

### 准备

1. 安装好 **MHWSS**，确认通过 `MHWSSLauncher.exe` 可以正常进入游戏。
2. 游戏开启 **DirectX 12** 和 **TAA**。
3. 退出游戏，备份游戏目录中已有的 `d3d12.dll`、`OptiScaler.ini`、`MHWFG.cmd` 和 `OptiScaler` 文件夹。已有 `d3d12.dll` 若属于其他模组，请先确认其兼容安装方式。

### 复制与启动

1. 从本仓库 **Releases** 下载 MHWFG 运行包并解压。
2. 将 `d3d12.dll`、`OptiScaler.ini` 和 `OptiScaler` 文件夹复制到 **`MonsterHunterWorld.exe` 所在目录**，合并文件夹并保留内部结构。`MHWFG.cmd` 是可选的启动快捷入口。
3. 运行原来的 **`MHWSSLauncher.exe`**，进入游戏。
4. 按 **Ins / Insert** 打开 MHWFG 菜单，选择 **2x**，勾选 **Active**。

安装后的主要目录如下：

```text
游戏目录/
├─ MonsterHunterWorld.exe
├─ MHWSSLauncher.exe
├─ MHWSS.dll
├─ MHWSS/
├─ d3d12.dll
├─ OptiScaler.ini
├─ MHWFG.cmd                 # 可选
└─ OptiScaler/
   ├─ nvngx_dlss.dll
   └─ streamline/
      ├─ sl.interposer.dll
      ├─ sl.common.dll
      ├─ sl.dlss_g.dll
      ├─ sl.pcl.dll
      ├─ sl.reflex.dll
      └─ nvngx_dlssg.dll
```

使用运行包配套的 DLL 和配置，保持以上目录结构。随包许可文件可以保留在解压目录。

## 使用

### 帧生成与倍数

- **Ins / Insert**：打开或关闭 MHWFG 菜单。
- **End**：打开 MHWSS 菜单。
- **Active**：勾选开启帧生成，取消勾选关闭。
- **Requested multiplier**：选择倍数，也可以在 Active 开启时切换。
- **Save Settings**：保存设置，供下次启动使用。

默认 Active 关闭、倍数为 2x。首页的 `Requested` 显示当前请求设置，实际输出受显卡、场景负载和外部模组设置影响。

### 限制帧率

在 **FPS Limit** 输入目标输出帧率，点击 **Apply Limit** 应用；点击 **Reset Limit** 恢复本模组的不限帧设置，`0` 表示不限帧。游戏和驱动的限帧设置各自独立。

高倍数输出超过显示器刷新率时可能产生撕裂，可降低倍数或设置合适的帧率上限。

### 界面与高级设置

- 点击 **Advanced** 查看完整 OptiScaler 设置，点击 **MHWFG** 返回首页。
- 拖动标题栏移动窗口，拖动右下角调整大小。
- **Menu Scale** 调整界面缩放；首页与 Advanced 分别记住当前会话中的窗口大小。

## 常见问题

**菜单打不开或帧生成没有生效？**

检查启动入口是否为 `MHWSSLauncher.exe`、游戏是否开启 DX12 和 TAA，并确认 `d3d12.dll` 位于游戏根目录、`OptiScaler` 子目录完整。打开本模组菜单的按键是 **Ins**。

**切换倍数后输出与预期不同？**

检查游戏、驱动和本模组的限帧设置；使用外部 MFG 模组时，也检查其倍数控制模式。可先回到 2x，关闭再开启 Active 进行对照。

**冰崖闪烁**

冰原部分冰崖和坡面在开启 Active 后可能出现白色或橙色闪烁。遇到时可暂时关闭 Active。

## 升级与卸载

- **升级**：退出游戏并备份旧文件，再复制新版运行文件。覆盖 `OptiScaler.ini` 会替换已有设置，请提前保存配置备份。
- **卸载**：退出游戏，恢复安装前备份；原本不存在的本模组文件可直接删除。共享文件夹内的 MHWSS、40MFG 和其他模组文件应保留。

## 反馈

提交 Issue 时请附上显卡型号、模组版本、是否安装 40MFG、复现步骤，以及相关截图或视频。提供日志前请检查其中的个人路径信息。

感谢 MHWSS、OptiScaler 及相关组件的开发者。

RTX 40 系使用 MFG，请前往 [dashdogy 的 RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock)，按作者提供的正常安装步骤安装，即可在本模组中开启 MFG。
