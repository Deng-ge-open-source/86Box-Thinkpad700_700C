# 86Box ThinkPad 700 / 700C

[English](README.md) | [简体中文](README.zh-CN.md)

这是一个基于 86Box 6.0 的实验性 IBM ThinkPad 700（9552 型）模拟项目。该分支增加了
ThinkPad 700 机型、固件可见的硬件行为、IBM 486SLC、MCA ESDI 启动链、单色 LCD
显示，以及运行 IBM 参考盘和同时代操作系统所需的输入与显示修复。

![在真正的 ThinkPad 700 中运行的 86Box ThinkPad 700 模拟环境](docs/images/thinkpad700-modernized-real-machine.jpg)

这就是本项目的实体 ThinkPad 700：外表保留经典原机外壳，内部换上现代
硬件，再由 86Box 重现它曾经的硬件与固件行为。它能像真实的 700 一样完成
POST、进入并操作 IBM 参考盘、执行自动配置、从 MCA ESDI 硬盘启动，并在真正的
ThinkPad 700 机身中运行同时代软件。它既保留了老机器的外观与操作体验，又让
内在拥有了现代、易于维护的第二次生命。

![在复刻机上运行的 IBM ThinkPad 700 PS/2 原版参考盘 1.20](docs/images/thinkpad700-original-reference-diskette.jpg)

这是 IBM ThinkPad 700 PS/2 原版参考盘 1.20 的启动画面，在这台复刻机上呈现出它
在真实 700 上运行时的样子。这套原厂配置与诊断环境会实际调用本项目希望保存的
各条固件路径。

![ThinkPad 700 参考盘](docs/images/thinkpad700-final-reference-disk.png)

> [!IMPORTANT]
> 本仓库只发布模拟器源代码，不发布 IBM BIOS ROM、参考盘、诊断盘、操作系统、硬盘
> 镜像或其他受版权保护的软件。请自行提供合法取得的副本。本机的 `local-assets/`
> 目录已被 Git 完整忽略。

## 项目状态

- 基线：86Box 6.0，上游提交 `4fef696a4`。
- 界面中的机型：`[MCA] IBM ThinkPad 700 (9552)`。
- 内部机型名：`ibmtp700`。
- 主要验证环境：IBM 参考盘 1.20，以及位于 370 柱面 MCA ESDI 镜像上的
  640×480 同时代 Windows 系统。
- 主要开发宿主：Linux ARM64；源代码仍保留 86Box 原有的 Windows 和 Linux
  跨平台构建方式。
- 当前属于实验性实现，尚未经过 86Box 上游审查，不应被视为周期级精确的硬件保存。

这是独立社区项目，与 IBM、Lenovo 或 86Box 上游项目不存在隶属或官方认可关系。

## 我们实现了什么

这次修改远不只是增加一项机型表。ThinkPad 固件会访问多条此前在 86Box 中不存在的
PS/2 与便携机专用硬件路径。

### ThinkPad 700 系统板与固件

- 增加 IBM ThinkPad 700 / 9552 机型和 `ibmtp700` 运行设备。
- 增加 700 单色机型使用的 `F1EE` Planar ID。现存的 `39G9996` 固件镜像属于
  700C（`F1EC`）；模拟器只在加载后的内存副本中转换六处 Planar ID 操作数，并修复
  受影响的两个 64 KiB 分区校验和，不修改用户的 ROM 文件。
- 实现该机型独有的 ROM shadow，以及位于 `FE0000h–FFFFFFh` 的正确 128 KiB
  高位 ROM 窗口，同时保留 POST 使用的完整低位 shadow。
- 实现固件与参考盘会查询的系统控制、VPD/串行 EEPROM、Planar 和存储状态寄存器。

### IBM 486SLC 与内存

- 增加 IBM 486SLC/25 选择与 ThinkPad 专用 CPU 配置。
- 实现 SLC 缓存控制寄存器，以及 POST 诊断使用的 16 KiB、四路组相联缓存标签行为。
- 在解释器和动态重编译器中补齐固件依赖的控制寄存器、堆栈、中断和返回指令路径。
- 修正 4 MiB 板载内存、扩展卡拓扑、16 MiB 存在位、保留区映射、NVRAM/EBDA、
  DMA 失效与热启动行为。

### MCA ESDI、系统分区与启动

- 扩展 MCA ESDI 控制器，加入 9552 固件需要的命令、几何参数、状态与电源管理行为。
- 支持实测镜像使用的 370 柱面、64 磁头、32 扇区几何参数。
- 实现参考盘到硬盘的启动交接，并记录导致 `I9990303` 的隐藏 3 MiB 系统分区与
  IML 要求。
- 增加 `tools/thinkpad700/patch_tp700_media.py`：它只处理本地工作副本，校验介质，
  必要时只恢复保留系统区，并在遇到非预期布局时拒绝继续。脚本本身不含 IBM 数据。

### 键盘、鼠标与软驱

- 修复 PS/2 指点设备初始化需要的 8042 命令参数提交顺序和 IRQ12 时序。
- 增加 ThinkPad POST 鼠标协商行为，并实现可靠的宿主鼠标释放。
- 实现诊断盘与参考盘会读取的 PS/2 软驱控制器电源/状态响应。

### LCD 与宿主集成

- 为 ThinkPad 700 路径增加固定 640×480 LCD 显示区域。
- 将 720×400 和 640×400 的固件/文本模式居中显示而不改变宿主窗口大小，并为
  POST、PC DOS 和参考盘文字保留安全边距。
- 增加最近邻灰阶、亮度、对比度、反色、全屏处理，以及面向无可用 GL 加速 ARM64
  宿主的轻量 SDL 软件贴图路径。
- 增加可选 JSON 状态输出，以及 `tools/thinkpad700/panel/` 下的实体 ThinkPad 风格
  LED/控制面板桥接程序。

### 已验证的固件结果

本地测试机已完成 POST、进入参考盘、完成自动配置、交接到硬盘并进入 Windows。
当前实现修复了导致 `00160`、`00164`、`00169`、`00201`、`12902`、`08611`、
`00191` 与 `I9990303` 的设备或配置问题；没有隐藏错误文字，也没有跳过对应的固件
检测。

原始验证细节见[开发记录](docs/DEVELOPMENT-NOTES.zh-CN.md)，通用 86Box 介绍见
[上游 README](docs/UPSTREAM_README.md)。

## 仓库结构

```text
src/                         86Box 源码与 ThinkPad 700 修改
cmake/, CMakeLists.txt       上游构建系统
configs/thinkpad700/         可公开的配置模板
docs/                        技术记录与精选截图
scripts/                     本地启动辅助脚本
tools/thinkpad700/           介质校验/修复与面板工具
local-assets/                被忽略的本地 ROM、介质和运行数据
build/                       被忽略的构建产物
```

`.gitignore` 会完整排除 `local-assets/`。原开发机上，该目录还保存迁移后的研究资料、
本地合法持有的介质和可运行环境。在单独确认许可证和再分发权之前，不要使用
`git add -f` 把其中内容加入提交。

## 需要用户自行提供的文件

机型启动至少需要合法取得的 256 KiB 固件镜像，并按以下 ROM 相对路径保存：

```text
roms/machines/ibmtp700/39G9996.bin
```

文件名与目录不可随意修改，因为 86Box 在代码中定义了 ROM 路径。你还需要自行提供
可启动软盘或硬盘镜像。若要复现实测参考盘流程，需要自己的参考盘镜像，以及含 IBM
系统分区的兼容 ESDI 硬盘镜像。

不要在唯一的原始介质上运行补丁工具。应使用副本，并保存原始文件的哈希和备份。

## 在 Linux 上构建

Ubuntu/Debian 可按 86Box 官方构建指南安装依赖：

```bash
sudo apt update
sudo apt install build-essential cmake extra-cmake-modules pkg-config \
  ninja-build libfreetype-dev libsdl2-dev libpng-dev libopenal-dev \
  librtmidi-dev libfluidsynth-dev libsndfile1-dev libserialport-dev \
  qtbase5-dev qtbase5-private-dev qttools5-dev libevdev-dev \
  libxkbcommon-dev libxkbcommon-x11-dev libslirp-dev
```

普通 x86-64 Linux 构建：

```bash
git clone https://github.com/Deng-ge-open-source/86Box-Thinkpad700_700C.git
cd 86Box-Thinkpad700_700C
cmake -S . -B build/linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/flags-gcc-x86_64.cmake
cmake --build build/linux -j"$(nproc)"
```

ARM64 Linux 宿主应启用新动态重编译器，可使用仓库自带预设：

```bash
cmake --preset flags-gcc-aarch64-regular
cmake --build out/build/flags-gcc-aarch64-regular -j"$(nproc)"
```

可执行文件通常位于相应构建目录的 `src/` 子目录。如果该预设不适合你的发行版，可
自行创建 Ninja 构建目录，并加入 `-DNEW_DYNAREC=ON`。

## 在 Windows 上构建

推荐在 **MSYS2 UCRT64 或 MINGW64** 终端中构建。先更新 MSYS2，再安装 86Box
官方构建指南列出的依赖：

```bash
pacman -Syu
pacman -S --needed git \
  $MINGW_PACKAGE_PREFIX-ninja \
  $MINGW_PACKAGE_PREFIX-cmake \
  $MINGW_PACKAGE_PREFIX-gcc \
  $MINGW_PACKAGE_PREFIX-pkgconf \
  $MINGW_PACKAGE_PREFIX-openal \
  $MINGW_PACKAGE_PREFIX-freetype \
  $MINGW_PACKAGE_PREFIX-SDL2 \
  $MINGW_PACKAGE_PREFIX-zlib \
  $MINGW_PACKAGE_PREFIX-libpng \
  $MINGW_PACKAGE_PREFIX-rtmidi \
  $MINGW_PACKAGE_PREFIX-fluidsynth \
  $MINGW_PACKAGE_PREFIX-libslirp \
  $MINGW_PACKAGE_PREFIX-libsndfile \
  $MINGW_PACKAGE_PREFIX-qt5-static \
  $MINGW_PACKAGE_PREFIX-qt5-translations \
  $MINGW_PACKAGE_PREFIX-vulkan-headers
```

在同一个 MSYS2 终端中构建：

```bash
git clone https://github.com/Deng-ge-open-source/86Box-Thinkpad700_700C.git
cd 86Box-Thinkpad700_700C
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows -j"$(nproc)"
```

完成后在 `build/windows/` 内查找 `86Box.exe`。把程序复制到其他 Windows 电脑时，
需要同时保留构建生成或 MSYS2 安装的运行库 DLL。Windows ARM64 需要上游的
LLVM/vcpkg ARM64 工具链和 Windows 11，与上述 MSYS2 x86-64 流程不同。

## 配置并启动虚拟机

可以导入 [`configs/thinkpad700/86box.cfg.example`](configs/thinkpad700/86box.cfg.example)，
也可以在图形界面中按下表创建：

| 设置 | 值 |
| --- | --- |
| 机型 | `[MCA] IBM ThinkPad 700 (9552)` |
| CPU | IBM 486SLC/25 |
| 内存 | 建议先用 4 MiB；已验证最大值为 16 MiB |
| 显示 | Internal |
| 鼠标 | PS/2 |
| 存储控制器 | Integrated MCA ESDI |
| 实测硬盘几何参数 | 370 柱面、64 磁头、32 扇区 |
| 软驱 | 3.5 英寸 1.44 MB |

可将 ROM 树放在虚拟机路径内，也可以显式指定：

```bash
./build/linux/src/86Box -R /path/to/rom-root /path/to/vm/86box.cfg
```

Windows PowerShell 的命令形式为：

```powershell
& .\build\windows\src\86Box.exe -R C:\path\to\rom-root C:\path\to\vm\86box.cfg
```

不同生成器的可执行文件子目录可能略有不同。`rom-root` 内必须包含
`roms/machines/ibmtp700/39G9996.bin`。

## 原开发机的一键启动

迁移后的 Linux 开发机仍可使用：

```bash
./scripts/launch-thinkpad700.sh
```

脚本使用被忽略的 `local-assets/legacy-work/` 历史运行环境，先验证/修复本地工作副本，
设置 ROM 路径，再启动现有 ARM64 构建。若本地数据放在其他位置，可设置
`TP700_WORK_ROOT=/another/path`。

也可以单独运行介质工具：

```bash
TP700_WORK_ROOT="$PWD/local-assets/legacy-work" \
  python3 tools/thinkpad700/patch_tp700_media.py
```

遇到非预期文件大小、字节、校验和或 FAT 布局时，脚本会主动停止。

## 实测 Linux 控制方式

- 点击客机画面捕获鼠标。
- `Ctrl+End` 释放鼠标；鼠标中键是备用方式。
- `Ctrl+Alt+Page Up` 切换 SDL 前端全屏。
- 使用窗口关闭按钮前，建议先退出全屏。

## 后续开发约定

今后的源码开发统一在本仓库进行。构建目录、ROM、虚拟机状态、磁盘镜像、日志和批量
抓帧应放在被忽略的目录。提交前至少执行：

```bash
git diff --check
cmake --build <你的构建目录> -j2
```

提交说明应指出对应的固件路径或硬件行为；验证材料应使用可复现日志或少量精选截图，
不要提交连续原始抓帧。

## 许可证与来源

本分支保留 86Box 的 GNU GPL v2-or-later 许可证和上游声明，详见
[`COPYING`](COPYING) 与 [`AUTHORS`](AUTHORS)。除非相关文件另有明确说明，对本仓库的
新贡献采用相同许可证。

上游参考资料：

- [86Box 项目](https://github.com/86Box/86Box)
- [官方构建指南](https://86box.readthedocs.io/en/stable/dev/buildguide.html)
- [ROM 搜索路径](https://86box.readthedocs.io/en/latest/usage/roms.html)
