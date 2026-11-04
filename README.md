# DesktopToggle

轻量的 Windows 11 x64 桌面开关工具，用于运行本地大模型等任务时释放桌面占用的显存和内存。当前版本为 **0.8**，可执行文件约 **17 KiB**。

程序暂时挂起当前会话的 `winlogon.exe`，关闭 `dwm.exe`、`explorer.exe`、`StartMenuExperienceHost.exe` 和 `ShellExperienceHost.exe`，再次切换时恢复桌面。实际释放量取决于系统和正在运行的程序，不保证释放全部显存。

## 使用方法

1. 运行 `dist/DesktopToggle.exe`，允许管理员权限请求。
2. 按 **Ctrl+Shift+Alt+M** 关闭桌面，再按一次恢复。
3. 桌面正常显示时，也可右键点击右下角托盘图标，选择“关闭桌面”或“退出程序”。正常退出会先恢复桌面。

关闭桌面后，任务栏和托盘会消失，请使用快捷键恢复，并保持本程序运行。程序不包含显存、内存监测或日志采集功能。

如默认快捷键被占用，可在启动时指定其他组合（先退出已有实例）：

```powershell
.\dist\DesktopToggle.exe --hotkey Ctrl+Shift+Alt+N
```

通过命令恢复桌面并退出正在运行的实例：

```powershell
.\dist\DesktopToggle.exe --stop
```

## 编译

安装 **Visual Studio 2022 Build Tools**，选择“使用 C++ 的桌面开发”，包含 MSVC x64 工具和 Windows SDK。

在项目根目录运行：

```powershell
.\build.cmd
```

产物为 `dist/DesktopToggle.exe`，中间文件位于 `build/`。脚本使用默认安装路径 `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`；若安装位置不同，请修改 `build.cmd` 中的 `vcvars64.bat` 路径。

源码位于 `src/main.cpp`，使用原生 Win32 API，无第三方运行时依赖。

## 许可证

Copyright 2026 Ranko-sama

本项目（含 v0.8 发布版本）采用 [Apache License 2.0](LICENSE) 开源协议。
