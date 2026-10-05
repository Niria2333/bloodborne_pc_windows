# Windows 移植总结

日期：2026-10-05。公开源码：[yaonikaixin999999/bloodborne_pc_windows](https://github.com/yaonikaixin999999/bloodborne_pc_windows)，Windows 分支为 `codex/windows-port`。

## 来源与范围

本项目以独立 GitHub 仓库发布，源码基于 [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) 的提交 `5224a6d137d8c4efaab69f2412f4bb86227b9ab8`，保留原作者版权、上游历史和许可证。上游已完成游戏专用运行库、离线加载准备、社区补丁应用以及基于 shadPS4 的 Vulkan 渲染器。此次工作将这些组件适配为原生 Windows x64 程序，并补充中文启动器及 Windows 验证。

这是源码发布。游戏本体、Sony 模块、生成的游戏镜像、存档、个人配置、开发工具安装目录和本机日志不属于公开发布内容；没有发布预编译 Windows 包。FSR 4 模型和着色器资源也不提交，仓库提供固定版本下载和校验工具。

## 已完成的实现

| 部分 | Windows 修改 |
| --- | --- |
| 构建 | MinGW UCRT64、CMake/Ninja 原生构建，静态链接游戏渲染器；运行 DLL 递归收集和许可证/哈希清单 |
| 运行库 | 保留 guest SysV ABI，Windows TLS、线程栈和 TEB 管理，线程/同步、文件、存档、时钟和系统服务 |
| 内存与异常 | Windows 占位符与稀疏共享映射、16 KiB guest 对齐、部分映射/解除映射、别名与保护；VEH 页面错误及投机访问恢复 |
| 视频 | 修复 AvPlayer 重复 join、停止/EOF 顺序、数据队列和重入锁问题，解决开场结束或跳过后黑屏 |
| 音频 | 用 SDL 精确等待替代 Windows 粗粒度等待，修复约 5.33 ms 缓冲周期被延迟并连续补发的问题 |
| 输入 | 保留原版按键，增加可选 Xbox 确认/返回映射；可见角色命名键盘及取消、重开、松开键、长度限制处理 |
| 启动器 | 中文界面；游戏语言自动/简体/繁体/英语、窗口/全屏、独立分辨率与质量、30/60/90/显示器节奏选项 |
| 画面设置 | FSR 3.1、FSR 4 v07 INT8、TAA、关闭超分、锐化、运动向量、响应遮罩、景深/模糊/SSAO/阴影/SSR/LOD；增量保存避免覆盖其他设置 |
| FSR 4 工具 | 固定 Q2RTX 源提交下载，模型/着色器大小和 SHA256 校验、SPIR-V 验证及 Windows 优化转换 |

Xbox 映射为 `A/B/X/Y → ○/✕/△/□`；角色命名键盘使用 A 选择、B 取消、X 删除、Y 或 Start 完成。中文需要游戏目录中已有相应语言资源，不向游戏安装新翻译数据。

## 实际结果

CUSA03023 1.09 已在 Windows 11、i5-13600KF、RTX 4070 Ti 12 GB、约 16 GB 系统内存的电脑上启动，经过开场和角色创建进入首个可玩区域。用户确认角色命名和进入游戏正常。1080p 预热后观察到 60 FPS，着色器编译时曾短暂下降至约 51～56 FPS。音频缓冲供给测试通过，观察区间没有缓冲耗尽；这不是完整听感质量评测。

最终构建验证通过 **16/16 CTest、74/74 Python 测试**。FSR 4 v07 下载器校验了 182 个上游资源，194 个 SPIR-V 校验通过；RTX 4070 Ti 上独立基准 `1280×720 → 1920×1080 Quality` 运行 8 帧并正常退出。详细范围见 [WINDOWS_VALIDATION.md](WINDOWS_VALIDATION.md)。

## 未完成与限制

FSR 4.1.1 Windows 适配未完成。本机没有 `VK_VALVE_shader_mixed_float_dot_product`，尚缺真实 4.1.1 捕获模型及可验证的便携着色器路径；不能把 FSR 4 v07 换名为 4.1.1，也不能仅取消支持检查。当前启动器保留旧配置标识但禁止新选择该模式，运行时不具备条件时回退 FSR 3.1。

4K 输出窗口和交换链初始化已验证，但 4K 关卡持续 60 FPS 尚未验证。FSR 4 游戏内、4K 和全预设测试、跨版本图像一致性、完整通关、长时间稳定性、其他电脑与全部手柄也未完成。30/90/显示器节奏选项仅验证参数和补丁一致性，尚未分别测量关卡帧率。

## 许可与贡献

本分支沿用 [GPL-2.0-or-later](../LICENSE)。上游 bbport、shadPS4 图形核心、FSR-Vulkan、AMD FidelityFX、Dear ImGui、LibAtrac9 等保留其署名和许可；社区帧率/图形补丁也保留原贡献者信息。代码发布包含构建脚本与测试，便于审查和复现。运行与构建方法见 [WINDOWS.md](WINDOWS.md)。
