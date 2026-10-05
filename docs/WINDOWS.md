# 血源 bbport：Windows 构建和运行

本项目以独立 GitHub 仓库发布，源码基于 [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)，保留原作者版权、上游提交历史和许可证，增加原生 Windows x64 运行库、Vulkan 渲染器构建和中文启动器。游戏进程无需 WSL。当前发布仅提供源码，没有预编译安装包或游戏数据。

已在一台 Windows 11 电脑上用 CUSA03023 1.09 进入实际关卡，1080p 预热后观察到 60 FPS。首次着色器编译会掉帧；4K 持续 60 FPS、完整通关和长期稳定性尚未验证。详细记录见 [WINDOWS_VALIDATION.md](WINDOWS_VALIDATION.md)。

## 环境要求

- Windows x64；已测试 Windows 11。内存后端使用现代 Windows 占位符 API，其他系统版本未验证。
- 支持 Vulkan 1.3 的显卡和驱动。当前验证机为 RTX 4070 Ti 12 GB、i5-13600KF、约 16 GB 系统内存，不代表最低配置。
- Git、Python 3（启动器需要 Tcl/Tk）和 MSYS2 UCRT64。当前构建验证使用 GCC 16.2、CMake 4.4、Ninja 和 Python 3.14；MSVC 构建未实现。
- 自己的已解密 Bloodborne 游戏目录，编号 CUSA03173 或 CUSA03023，版本 1.09。

## 获取和构建源码

```powershell
git clone --branch codex/windows-port --recurse-submodules https://github.com/yaonikaixin999999/bloodborne_pc_windows.git
cd bloodborne_pc_windows
```

已经克隆但子模块不完整时执行 `git submodule update --init --recursive`。在 **MSYS2 UCRT64** 终端安装依赖：

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-sdl3 \
  mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader \
  mingw-w64-ucrt-x86_64-vulkan-memory-allocator mingw-w64-ucrt-x86_64-ffmpeg \
  mingw-w64-ucrt-x86_64-boost mingw-w64-ucrt-x86_64-fmt \
  mingw-w64-ucrt-x86_64-robin-map mingw-w64-ucrt-x86_64-xxhash \
  mingw-w64-ucrt-x86_64-zydis mingw-w64-ucrt-x86_64-glslang \
  mingw-w64-ucrt-x86_64-spirv-tools mingw-w64-ucrt-x86_64-spirv-cross \
  mingw-w64-ucrt-x86_64-python
```

然后在源码目录的 PowerShell 执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_windows.ps1 -MsysRoot "C:\msys64" -Test
```

将 `C:\msys64` 换成实际安装位置。脚本也会自动探测默认目录及源码上级目录的 `tools-local/msys64`。`-Test` 运行 CTest 和七组 Python 测试，任一失败都会停止；仅编译时省略它。默认并行 4 个任务，可用 `-Jobs 3` 降低内存压力，`-BuildDir` 指定其他构建目录。`-Diagnostics` 仅构建诊断后端，不能玩游戏。

CMake 首次配置会下载固定版本的 magic_enum、xbyak、miniz 等依赖，需要联网；已有 `out/dependency-sources/` 缓存时构建脚本可以复用。完整产物为 `out/windows/bin/bb-probe.exe`。

## 准备游戏目录

选择直接包含 `eboot.bin` 的目录，例如 `C:\Games\CUSA03023`：

```text
CUSA03023/
  eboot.bin
  sce_sys/param.sfo
  sce_module/
    libc.prx
    libSceFios2.prx
    ...
  dvdroot_ps4/
    chr/
    map/
    msg/
    ...
```

启动器读取 `param.sfo`，检查 `TITLE_ID=CUSA03173` 或 `CUSA03023`、`APP_VER=01.09` 或 `1.09`，准备脚本继续验证可加载代码。编号是 PS4 游戏发行版本标识，文件夹名称本身不能改变编号。无需重命名 CUSA03023，也不要选择它的上级目录。更新须已合并为 1.09；程序不下载游戏或系统模块。

## 启动与设置

```powershell
python .\run_windows.py --gui
```

也可以双击源码根目录的 `start_windows.cmd`。在“启动”页选择游戏目录、输出分辨率、60 帧、语言及窗口/全屏；“画面设置”页调整超分和特效。首次建议使用 1080p、FSR 3.1，进入关卡后再提高负载。首次启动会生成加载镜像、链接游戏自带模块并应用补丁。

游戏语言支持 `auto/zh-cn/zh-tw/en`。自动模式优先检测 `dvdroot_ps4/msg/zhocn` 简体资源，再检测 `zhotw` 繁体资源，最后英语；明确选择中文但资源缺失时会报错。全屏勾选框使用无边框全屏。

手柄可选原版映射或 Xbox 映射。亚洲版游戏通常使用 ○ 确认、✕ 返回：原版对应 Xbox B/A；Xbox 方案为 `A/B/X/Y → ○/✕/△/□`。角色命名屏幕键盘使用 A 选择、B 取消、X 删除、Y 或 Start 完成。键盘菜单方向为 I/K/J/L，左 Shift 为 ○，空格为 ✕。

```powershell
python .\run_windows.py --game "C:\Games\CUSA03023" --resolution 1080p --fps 60 --language zh-cn --controller-layout xbox --windowed
python .\run_windows.py --game "C:\Games\CUSA03023" --resolution 4k --fullscreen
python .\run_windows.py --game "C:\Games\CUSA03023" --check
python .\run_windows.py --game "C:\Games\CUSA03023" --prepare-only
```

省略语言、帧率、手柄方案和显示模式会沿用已保存选择；省略 `--resolution` 会保留 `bbport.ini` 的自定义画质。显式分辨率档位只更新输出、预设与缩放路径，不重置其他效果。GUI 保存只提交用户修改字段，避免覆盖游戏内设置。

## 分辨率、超分和帧率

GUI 的输出尺寸与质量档位独立，可选 720p、1080p、1440p 和 4K。CLI 另提供便捷档位：

| `--resolution` | 输出 | 预设 |
| --- | --- | --- |
| `1080p` | 1920×1080 | Quality |
| `1440p` | 2560×1440 | Quality |
| `4k` | 3840×2160 | Balanced |
| `4k-quality` | 3840×2160 | Quality |
| `4k-native` | 3840×2160 | Native AA |

FSR 3.1 与 FSR 4 的 Quality/Balanced 等预设以较低场景分辨率重建输出；Native AA 使用原生场景尺寸。TAA 和关闭超分也使用原生场景尺寸。4K 输出不等于原生 4K 场景渲染，具体尺寸会显示在设置页。1080p 使用渲染器的动态缩放路径；其他输出默认通过启动补丁设置场景尺寸。

可选 FSR 3.1、FSR 4 v07 INT8、TAA 或关闭超分。**FSR 4.1.1 Windows 适配尚未完成，启动器禁止新选。** FSR 4 还需模型资源及 GPU 必需特性；独立基准通过不代表游戏内各预设已经验证。没有新增 DLSS 或帧生成实现。

帧率选项为 `--fps 30|60|90|uncap`。30/60 使用 60 Hz vblank，90 使用 90 Hz；`uncap` 使用显示器节奏（当前上游实现最高 120 帧）。90 和 `uncap` 标记为实验功能，目前仅验证参数/补丁一致性。60 帧目标对应约 16.7 ms 每帧，补丁不能保证每个场景均达到目标。

画面设置还包括锐化、运动向量、响应遮罩、景深、运动模糊、SSAO、动态阴影、SSR、模型 LOD、显示帧率和跳过开场。进入游戏后可用 **Insert** 或 **L3+R3** 打开移植设置菜单；部分分辨率/预设变化需“应用并重启”，启动器会接收重启请求并保留选择。

## 可选 FSR 4 v07 资源

FSR 3.1 无需单独模型。FSR 4 资源不在源码或本地 DLL 包中；在源码目录执行：

```powershell
python .\tools\fetch_fsr4_assets_windows.py --tools-dir "C:\msys64\ucrt64\bin"
```

下载器固定使用 Q2RTX 提交 `ae8d628fae208813172446d1e49ed94150b04658` 的 MIT 资源，涵盖六种预设及 1080/2160 tier，校验上游 manifest 的文件大小/SHA256、SPIR-V，并生成优化着色器。报告为 `fsr4_shaders/ASSET_INSTALL_REPORT.json`；该目录被 Git 忽略。工具使用 MSYS2 中的 `spirv-val` 等程序，请按实际安装位置设置 `--tools-dir`。下载工具不执行 GPU 测试。

FSR 4.1.1 上游捕获工具需要 Proton。本分支尚未完成原生 Windows 捕获/模型转换和对应 GPU 验证，不能用 v07 资源代替 4.1.1。

## 数据与本地运行包

| 路径 | 用途 |
| --- | --- |
| `bbport.ini` | 图形与游戏效果设置 |
| `user/launcher.json` | 游戏路径、语言、帧率、窗口和手柄选择 |
| `user/` | 存档、缓存和运行日志 |
| `out/windows-data/` | 从游戏生成的镜像、补丁和准备报告 |
| `out/windows-data/last-run.log` | GUI 启动日志 |
| `out/windows/` | 完整构建产物 |
| `dist/windows/` | 可选本地 DLL 运行包 |
| `fsr4_shaders/` | 单独下载的 FSR 4 资源 |

生成镜像含游戏代码，存档和配置也属于本机数据，不应加入源码提交。默认使用独立 `user/`，不会直接写入其他模拟器的存档目录。

本地打包命令（按实际 MSYS2 位置替换 `--prefix`）：

```powershell
python .\scripts\package_windows.py --prefix "C:\msys64\ucrt64"
```

脚本递归检查 PE 导入，收集所需 DLL 和许可，生成 `dependencies.json` 与 `SHA256SUMS.txt`。启动器优先使用 `dist/windows/bb-probe.exe`；修改代码后需重新打包，否则可能启动旧程序。不要只移动 EXE；Python 启动器仍需要源码中的脚本和补丁。这里的本地打包不表示仓库已提供二进制下载。

## 排查问题

启动失败时先查看 `out/windows-data/last-run.log`。缺游戏文件时检查所选目录及结构；版本拒绝时检查 `param.sfo` 和更新合并；DLL 缺失时从源码启动器运行，并确认 MSYS2 路径或本地 DLL 包完整。

报告运行错误时附上显卡/驱动、当前输出和超分设置、场景、是否可复现及相关日志。发布日志前移除个人路径；不要附游戏文件或生成镜像。

Windows 稀疏共享内存使用 `SEC_RESERVE`，已提交页的内存承诺保留到进程退出，释放/复用时清零。长时间内存峰值和高负载 4K 表现仍需实测。测试通过、窗口初始化通过和低负载菜单 60 FPS 都不能代替关卡及长期运行验证。
