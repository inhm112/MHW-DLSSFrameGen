# MHWFG

[简体中文](README.md) | **English**

> This mod does not provide any MHWSS files. Please visit [the original author's Patreon page](https://www.patreon.com/huutaiii/posts/mhwss-v1-0-2-mod-142322059) to obtain MHWSS and support the original author.

Provides **DLSS Frame Generation (FG) and Multi Frame Generation (MFG)** for *Monster Hunter: World / Iceborne*, used alongside MHWSS and based on a modified version of [OptiScaler](https://github.com/optiscaler/OptiScaler).

## Features

- Frame generation toggle and **2–6x** fixed multiplier selection. Available multipliers depend on the GPU and runtime component capabilities.
- A streamlined MHWFG home page with frame rate limiting, configuration saving, and UI scaling.
- **Advanced** retains the complete OptiScaler settings.
- Move the window by dragging it, and resize it by dragging the bottom-right corner.

## Installation

### Preparation

1. Install **MHWSS** and confirm that you can enter the game normally through `MHWSSLauncher.exe`.
2. Enable **DirectX 12** and **TAA** in the game.
3. Exit the game and back up any existing `d3d12.dll`, `OptiScaler.ini`, `MHWFG.cmd`, and `OptiScaler` folder in the game directory. If an existing `d3d12.dll` belongs to another mod, first confirm a compatible installation method.

4. Check for `nvngx_dlisp.dll` in the game root directory. This legacy NGX component causes Streamline initialization errors and **Not ready** in the reproduced configurations. If present, rename it to `nvngx_dlisp.dll.disabled` to keep a backup before launching. Do not rename `nvngx_dlss.dll` or `nvngx_dlssg.dll`.

### Copying Files and Launching

1. Download the MHWFG runtime package from this repository's **Releases** and extract it.
2. Copy `d3d12.dll`, `OptiScaler.ini`, and the `OptiScaler` folder into **the directory containing `MonsterHunterWorld.exe`**, merging folders and preserving their internal structure. `MHWFG.cmd` is an optional launcher shortcut.
3. Run the original **`MHWSSLauncher.exe`** to enter the game.
4. Press **Ins / Insert** to open the MHWFG menu, select **2x**, and check **Active**.

The main directory structure after installation is as follows:

```text
Game directory/
├─ MonsterHunterWorld.exe
├─ MHWSSLauncher.exe
├─ MHWSS.dll
├─ MHWSS/
├─ d3d12.dll
├─ OptiScaler.ini
├─ MHWFG.cmd                 # Optional
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

Use the DLLs and configuration supplied with the runtime package, preserving the directory structure above. The included license files can remain in the extraction directory.

## Usage

### Frame Generation and Multipliers

- **Ins / Insert**: Open or close the MHWFG menu.
- **End**: Open the MHWSS menu.
- **Active**: Check to enable frame generation; uncheck to disable it.
- **Requested multiplier**: Select a multiplier. You can also switch it while Active is enabled.
- **Save Settings**: Save settings for the next launch.

By default, Active is disabled and the multiplier is 2x. `Requested` on the home page shows the current requested settings; actual output is affected by the GPU, scene workload, and external mod settings.

### Frame Rate Limiting

Enter the target output frame rate in **FPS Limit** and click **Apply Limit** to apply it. Click **Reset Limit** to restore this mod's unlimited frame rate setting; `0` means unlimited. The game's and driver's frame rate limits are independent settings.

High-multiplier output exceeding the monitor's refresh rate may cause tearing. You can lower the multiplier or set an appropriate frame rate limit.

### Interface and Advanced Settings

- Click **Advanced** to view the complete OptiScaler settings, or **MHWFG** to return to the home page.
- Drag the title bar to move the window, and drag the bottom-right corner to resize it.
- **Menu Scale** adjusts UI scaling. The home page and Advanced each remember their window size for the current session.

## Frequently Asked Questions

**The menu does not open, or frame generation is not working?**

Check that you are launching through `MHWSSLauncher.exe`, that DX12 and TAA are enabled in the game, that `d3d12.dll` is in the game root directory, and that the `OptiScaler` subdirectory is complete. The key to open this mod's menu is **Ins**.

**DLSS works, but FG shows Not ready and Active is unavailable?**

One confirmed trigger is the game-root `nvngx_dlisp.dll` (identified version `1.2.17.0`): in the affected combination, it raises an exception that prevents the FG swapchain from being established while DLSS continues to work. This file is not distributed with this mod. An RTX 4060 Laptop crash dump and a single-variable reproduction on an RTX 4070 Ti SUPER point to the same location in this module.

Exit the game and rename `nvngx_dlisp.dll` next to `MonsterHunterWorld.exe` to `nvngx_dlisp.dll.disabled`. Keep the file and relaunch. To revert, exit the game and restore its original name. After Steam verifies game files or the game updates, check whether the original filename has been restored.

This is not the only possible cause of Not ready. If the file is already disabled and the issue persists, provide a fresh log for further diagnosis.

**Output differs from expectations after switching multipliers?**

Check the frame rate limits in the game, driver, and this mod. If you use an external MFG mod, also check its multiplier control mode. You can first return to 2x, then switch Active off and on for comparison.

**Ice cliff flickering**

Some ice cliffs and slopes in Iceborne may show white or orange flickering when Active is enabled. If this occurs, you can temporarily disable Active.

## Updating and Uninstalling

- **Updating**: Exit the game and back up the old files before copying the new runtime files. Overwriting `OptiScaler.ini` replaces your existing settings, so back up your configuration beforehand.
- **Uninstalling**: Exit the game and restore your pre-installation backups. Files added by this mod that did not previously exist can be deleted. Keep MHWSS, 40MFG, and other mods' files in shared folders.

## Feedback

When submitting an Issue, include your GPU model, mod version, whether 40MFG is installed, reproduction steps, and relevant screenshots or videos. Check logs for personal path information before sharing them.

Thanks to the developers of MHWSS, OptiScaler, and the related components.

To use MFG on an RTX 40 Series GPU, visit [dashdogy's RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock) and follow the author's standard installation instructions. You can then enable MFG in this mod.
