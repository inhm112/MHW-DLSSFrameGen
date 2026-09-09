# 构建 MHWFG

## 环境

- Windows x64，PowerShell 7.6.4 或更新版本。
- Visual Studio 2022 C++ 工具链：v143、Windows SDK 10.0.26100.0。
- 本轮验证：PowerShell 7.6.5，MSVC 14.44.35207，MSBuild 17.14.51。

## 构建命令

在源码根目录运行：

```powershell
$env:MSBUILDDISABLENODEREUSE = '1'
pwsh ./production/tools/Build-MHWFG.ps1 -SourceRoot . -OutputDirectory ../mhwfg-build
```

输出目录使用源码目录之外的专用文件夹。含空格的路径请加引号。

脚本自动查找 MSBuild，也可通过 `-MSBuildPath` 指定其路径。构建使用 Release|x64 与 `MHWFGProduction=true`，关闭上游 Pre/PostBuild 事件。完整重建会清理输出目录内的构建产物，并覆盖构建日志。

产物位于 `../mhwfg-build/bin/OptiScaler.dll`；安装到游戏时使用文件名 `d3d12.dll`。日志位于输出目录的 `build-mhwfg.log`。

## 源码目录

`OptiScaler/` 是修改后的产品源码，`external/` 保存依赖材料，`production/` 包含配置、说明与构建脚本。当前构建沿用上游提供的预编译库，保留完整后端。源码快照排除了 SDK 示例可执行程序、运行 DLL 和工具二进制，使用已有着色器头文件完成上述构建；重新生成着色器时需自行准备上游脚本要求的编译工具。

上游基线和依赖来源见 `production/public/SOURCES.md`；上游原说明保存在 `UPSTREAM_README.md`。
