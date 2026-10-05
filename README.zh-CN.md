# 血源 bbport：Windows 实验移植

[English](README.md) · [构建和运行说明](docs/WINDOWS.md) · [验证记录](docs/WINDOWS_VALIDATION.md) · [移植总结](docs/WINDOWS_PORT_SUMMARY.md)

这是发布在 [yaonikaixin999999](https://github.com/yaonikaixin999999/bloodborne_pc_windows) 账号下的独立开源项目，基于 [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) 增加 Windows x64 适配。保留原作者版权、完整上游提交历史、Linux 实现和第三方署名，在其原生游戏运行库与 Vulkan 渲染器上增加 Windows 支持。它针对《血源》1.09 的原始 x86-64 游戏程序工作，不是通用 PS4 模拟器。

**当前为实验版本，已在一台 Windows 11 电脑上进入实际关卡。** CUSA03023 1.09 已经过开场、中文界面和角色命名，用户确认能够正常命名并进入游戏。1080p 关卡在着色器缓存预热后观察到 60 FPS；首次编译着色器时有短暂掉帧。完整通关、真实游戏存档反复读写兼容性、长时间稳定运行和 4K 持续 60 FPS 尚未验证。

本分支已完成：

- 原生 MinGW UCRT64 构建，以及 Windows 线程、TLS、内存映射、异常恢复、文件和存档接口。
- Windows SDL3/Vulkan 窗口输出；修复开场视频停止/结束时的线程竞争，以及音频等待导致的长停顿和缓冲突发。
- 中文启动器，支持游戏语言自动检测、简体/繁体/英语、窗口/全屏、可选 Xbox 按键映射及手柄操作的角色命名键盘。
- 独立分辨率与画质档位，FSR 3.1、可选 FSR 4 v07 INT8、TAA、特效和模型细节设置；30/60/90/跟随显示器帧率选项，后两项为实验功能。

**FSR 4.1.1 的 Windows 适配尚未完成，启动器不能新选该模式。** FSR 4 v07 INT8 已完成资源校验和一次独立 1080p 基准运行，游戏内及 4K FSR 4 表现仍需验证，详情见验证记录。

**本次开源发布仅含源码，没有预编译 Windows 安装包，也没有游戏数据。** 运行需要自行准备已解密的 CUSA03173 或 CUSA03023、版本 1.09 的游戏目录。FSR 3.1 无须额外模型，FSR 4 模型/着色器使用仓库中的 Windows 工具单独下载。

```powershell
git clone --branch codex/windows-port --recurse-submodules https://github.com/yaonikaixin999999/bloodborne_pc_windows.git
cd bloodborne_pc_windows
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_windows.ps1 -MsysRoot "C:\msys64" -Test
python .\run_windows.py --gui
```

先按[构建说明](docs/WINDOWS.md)安装 Python、MSYS2 UCRT64 和依赖。自动验证已通过 **16/16 项 CTest 和 74/74 项 Python 测试**。这表示被测试的接口和配置流程通过，不代表全游戏或所有电脑都能稳定运行。

项目沿用上游 **GPL-2.0-or-later** 许可，完整文本见 [LICENSE](LICENSE)。图形实现基于 [shadPS4](https://github.com/shadps4-emu/shadPS4)，FSR-Vulkan、AMD FidelityFX、Dear ImGui、LibAtrac9 等依赖保留各自许可和署名。Windows 修改不代表整个项目从零原创；上游作者及社区补丁贡献者的工作仍是本项目基础。

本项目与 Sony Interactive Entertainment、FromSoftware 或 AMD 无关联。原始 Linux 说明保留在 [英文 README](README.md) 下半部分，其中 Linux 性能数字与 FSR 4.1.1 结果不能当作 Windows 验证结果。
