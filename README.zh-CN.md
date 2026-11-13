# 86Box ThinkPad 700 / 700C

[English](README.md) | [简体中文](README.zh-CN.md)

这是一个基于 86Box 6.0 的实验性 IBM ThinkPad 700 / 700C（9552 型）模拟项目。该分支增加了
ThinkPad 700 / 700C 机型、固件可见的硬件行为、IBM 486SLC、MCA ESDI 启动链、单色与彩色 LCD
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
> 镜像或其他受版权保护的软件。请自行提供合法取得的副本。

## 项目状态

- 基线：86Box 6.0，上游提交 `4fef696a4`。
- 界面中的机型：`[MCA] IBM ThinkPad 700 (9552)` 和 `[MCA] IBM ThinkPad 700C (9552)`。
- 内部机型名：`ibmtp700` 和 `ibmtp700c`，分别使用独立的 BIOS/NVRAM 名称。
- 主要验证环境：IBM 参考盘 1.20，以及位于 370 柱面 MCA ESDI 镜像上的
  640×480 同时代 Windows 系统。
- 早期开发宿主：Linux ARM64；700C 路径也已在 Windows x64 验证。
  源代码仍保留 86Box 原有的 Windows 和 Linux 跨平台构建方式。
- 当前属于实验性实现，尚未经过 86Box 上游审查，不应被视为周期级精确的硬件保存。

这是独立社区项目，与 IBM、Lenovo 或 86Box 上游项目不存在隶属或官方认可关系。

## 本次更新 — 2026-11-13

- 700C 独立固件身份、对应参考盘和 256 色 Windows 支持。
- 修正 LCD 画面比例，使用纯键盘调节亮度/对比度，不增加屏幕控件。
- 固件/存储修复、带日期的启动验证，以及带全新 README 的可复现私人交接包。

具体修改与验证边界见[更新记录](docs/RELEASE-NOTES.md)。

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

### ThinkPad 700C 彩色支持

- 增加独立 F1EC 机型，直接使用未经修改的 256 KiB `39G9996` 原始固件，
  使用独立 BIOS/NVRAM 名称及对应的 700C 参考盘。
- 修复 VPD/系统身份校验、空错误日志初始化、MCA ESDI 仲裁级别分配和
  READ BUFFER 续传；保留已有的非空错误日志。
- 增加 IBM 256 色 Windows 驱动使用的 512 KiB VGA256C 显示路径、90FB 显卡
  POS 身份和分银行的紧凑像素显存。
- 在 640×480 彩色面板中居中显示 400 行文本，提供可选纵向展开与纯键盘
  亮度/对比度调节。
- 正常 CPU 执行不再进行全内存 IML 诊断扫描，保留原始固件 POST 检测。

### 已验证的固件结果

本地测试机已完成 POST、进入参考盘、完成自动配置、交接到硬盘并进入 Windows。
当前实现修复了导致 `00160`、`00164`、`00169`、`00201`、`12902`、`08611`、
`00191` 与 `I9990303` 的设备或配置问题；没有隐藏错误文字，也没有跳过对应的固件
检测。

700C 还已验证自动配置、版本与身份页面、空错误日志，以及 Windows 3.2 标准模式的
640×480、8 位扫描和 512 KiB 显存。Windows 增强模式、扩展视频加速和完整 IBM
硬件诊断套件尚未验证，详见带日期的[验证记录](docs/VERIFICATION.md)。

原始验证细节见[开发记录](docs/DEVELOPMENT-NOTES.zh-CN.md)，通用 86Box 介绍见
[上游 README](docs/UPSTREAM_README.md)。

## 仓库结构

```text
src/                         86Box 源码与 ThinkPad 700/700C 修改
cmake/, CMakeLists.txt       上游构建系统
configs/thinkpad700/         可公开的单色配置模板
configs/thinkpad700c/        可公开的彩色配置模板
docs/                        技术记录与精选截图
scripts/                     本地启动辅助脚本
tools/thinkpad700/           介质校验/修复与面板工具
local-assets/                被忽略的本地 ROM、介质和运行数据
build/                       被忽略的构建产物
private-dist/                被忽略的私人运行包与交接包
```

`.gitignore` 会完整排除 `local-assets/`。原开发机上，该目录还保存迁移后的研究资料、
本地合法持有的介质和可运行环境。在单独确认许可证和再分发权之前，不要使用
`git add -f` 把其中内容加入提交。

## 需要用户自行提供的文件

### ThinkPad 700 单色配置

机型启动至少需要合法取得的 256 KiB 固件镜像，并按以下 ROM 相对路径保存：

```text
roms/machines/ibmtp700/39G9996.bin
```

文件名与目录不可随意修改，因为 86Box 在代码中定义了 ROM 路径。你还需要自行提供
可启动软盘或硬盘镜像。若要复现实测参考盘流程，需要自己的参考盘镜像，以及含 IBM
系统分区的兼容 ESDI 硬盘镜像。

不要在唯一的原始介质上运行补丁工具。应使用副本，并保存原始文件的哈希和备份。

### ThinkPad 700C 彩色配置

| 配置 | ThinkPad 700 | ThinkPad 700C |
| --- | --- | --- |
| 界面机型 | [MCA] IBM ThinkPad 700 (9552) | [MCA] IBM ThinkPad 700C (9552) |
| 内部名称 | `ibmtp700` | `ibmtp700c` |
| Planar 身份 | F1EE | F1EC |
| ROM 根目录下的路径 | `machines/ibmtp700/39G9996.bin` | `machines/ibmtp700c/39G9996.bin` |
| 参考盘 1.20 | `r700m120` | `r700c120` |
| 系统程序 | `$0007000.IML` | `$0009000.IML` |
| 配置 | [700 模板](configs/thinkpad700/86box.cfg.example) | [700C 模板](configs/thinkpad700c/86box.cfg.example) |

现存 256 KiB `39G9996` 固件转储属于 700C。700C 直接使用原始内容；700 路径只在
**加载后的内存副本**中转换系统板 ID 操作数并修复校验和，不修改用户 ROM 文件。

700C 参考盘包含 `PF1EC.ADF`、`SF1EC.ADF` 和 `$0009000.IML`。
两个机型的可写介质和 NVRAM 应分开，不要混用单色参考盘，也不要对 700C 介质运行
700 专用修复脚本。另需正常的 86Box 字体 ROM 树；实测本地包的 ROM 根目录包含
`video/mda/mda.rom`。

#### 配置和启动

将对应的 700C 参考盘保持挂载在 A:，冷启动先进入参考盘，再选择
**Start operating system** 交接到硬盘。需要 IBM 隐藏系统分区及对应的 IML 程序，
仅有普通 DOS 硬盘不能复现这条启动链。

首次配置或转换 700C 硬盘工作副本时，在 700C 参考盘中选择
**Backup/Restore system programs → Restore the system partition**，
按提示提供 700/700C/720/720C 通用诊断盘，然后执行
**Set configuration → Run automatic configuration**。
只处理工作副本，保留原始镜像。

#### 在 Windows 上从 USB 实体软盘启动

Windows 构建可以通过宿主软驱读取标准的 720 KiB、1.44 MB 或 2.88 MB 3.5 英寸软盘，
实际可用格式取决于软驱。模拟机始终将实体盘视为**写保护**状态，尚未实现向实体盘写入
或格式化。使用与机型匹配的 Reference Diskette，也可以使用可启动的 DOS 软盘。

若宿主软驱盘符是 `A:`，在虚拟机已有的 `[Floppy and CD-ROM drives]` 节中设置：

```ini
fdd_01_type = 35_2hd
fdd_01_fn = ioctl://\\.\A:
fdd_01_host_device = \\.\A:
fdd_host_buffering = 1
```

冷启动优先通过模拟软驱控制器读取实体盘；若无法挂载实体盘，ThinkPad 完成自检后
启动已配置的硬盘镜像，下次冷启动会重新尝试实体软驱。已经挂载但不能启动的软盘
仍由原固件报错，读取失败不会当作启动成功。宿主 Windows 需要先运行，700/700C ROM
仍在 86Box 内执行。
实体软驱会自动关闭模拟软驱音效；硬盘音效和镜像软盘音效保留
原配置。在 Windows 中选用实体软驱时，硬盘音效线程按宿主时间续播，避免同步 USB
读取打断主轴声；暂停虚拟机仍会暂停声音生成。读取缓存会在内存中保留访问过的磁道，
Windows 报告换盘时自动失效。
更换软盘后，也可通过 **介质 → 软驱 → Use Host Floppy Drive** 重新挂载，或重启
虚拟机，显式清除缓存。
Windows 的设备访问可能需要管理员权限。这项功能不包含 USB 设备直通或实体软驱的
底层硬件诊断。
已在 Windows x64 上使用 TEAC USB UF000x 软驱，从实体 700C 参考盘启动到主菜单，
详见[实体介质验证记录](docs/VERIFICATION.md#windows-physical-usb-floppy--2026-11-13)。

#### 安装 700C Windows 彩色驱动

**700C120 VGA256C 1.20 驱动盘**与 **R700C120 参考盘**是不同的介质。
[IBM 原始驱动安装说明](https://basterfield.com/pccbbs/mobiles/700c120.txt)
要求在 Windows 前加载 `VESA.COM`，并选择
**640x480x256 Small fonts 512k IBMVGA256c**。

本地 Windows 3.2 标准模式已核对为 640×480、8 位扫描、512 KiB 显存。
画笔中已显示自行生成的 256 色测试位图；Windows 保留调色板会合并部分输入颜色。

![700C 运行彩色 Windows 3.2](docs/images/thinkpad700c-windows-color.png)

![画笔显示 256 色测试图](docs/images/thinkpad700c-256-color-test.png)

#### 彩色 LCD 画面比例

700C 直接使用客机 VGA 调色板，不叠加灰阶或单色染色。
物理面板以固定 640×480、4:3 显示区域模拟。默认将参考盘的 640×400 文本居中，
上下各留 40 行；Windows 的真正 640×480 图形铺满面板。

如需可选的文本展开效果，可在 700C 机型配置启用 **LCD vertical expansion**
（`lcd_vertical_expansion = 1`）。提供的模板使用 `0`，
与 [IBM Quick Reference](https://macdat.net/files/pdf/ibm/user_manuals/thinkpad_700_ps2_quick_reference.pdf)
中的默认设置一致。另见 [IBM 原始规格](https://oldthinkpad.com/static/pdf/ibm-thinkpad700xps2.pdf)。

![700C 参考盘在彩色 LCD 面板中居中显示](docs/images/thinkpad700c-reference-proportions.png)

#### 固件检查与启动耗时

实测配置下，700C 参考盘的 **More utilities → Display system error log**
可以正常打开，并报告错误日志为空。

![700C 参考盘确认错误日志为空](docs/images/thinkpad700c-error-log-empty.png)

16 MiB 配置实测完整 POST 约 38 秒，其中 CP52 内存检查约 32 秒。CP67 在本地
计时中不到 1 毫秒便通过，未复现间歇性卡死。这些 CP 值是固件进度检查点，
原始检测仍保留。

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
  $MINGW_PACKAGE_PREFIX-libserialport \
  $MINGW_PACKAGE_PREFIX-qt5-static \
  $MINGW_PACKAGE_PREFIX-qt5-translations \
  $MINGW_PACKAGE_PREFIX-vulkan-headers
```

在同一个 MSYS2 终端中构建：

```bash
git clone https://github.com/Deng-ge-open-source/86Box-Thinkpad700_700C.git
cd 86Box-Thinkpad700_700C
cmake -S . -B build/windows-700c --preset regular \
  --toolchain ./cmake/flags-gcc-x86_64.cmake \
  -DNEW_DYNAREC=OFF -DQT=ON
cmake --build build/windows-700c -j"$(nproc)"
```

完成后在 `build/windows-700c/` 内查找 `86Box.exe`。动态构建还需要对应运行库 DLL。
[Windows 发布工作流](.github/workflows/windows_x64_release.yml)列出了实测 UCRT64
依赖，公开便携产物不包含固件或已安装客机。CI 也通过 MSYS2 CLANGARM64 和新
重编译器构建 Windows ARM64；ARM64 运行行为尚未验证。

## 配置并启动虚拟机

可以导入 [`configs/thinkpad700/86box.cfg.example`](configs/thinkpad700/86box.cfg.example)，
彩色机型则使用 [700C 模板](configs/thinkpad700c/86box.cfg.example)。
通用硬件配置如下：

| 设置 | 值 |
| --- | --- |
| 机型 | `[MCA] IBM ThinkPad 700 (9552)` 或 `[MCA] IBM ThinkPad 700C (9552)` |
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
& .\build\windows-700c\src\86Box.exe -R C:\path\to\rom-root C:\path\to\vm\86box.cfg
```

不同生成器的可执行文件子目录可能略有不同。`rom-root` 直接指向 ROM 目录
（例如 `runtime/roms`），内含 `machines/ibmtp700/39G9996.bin`，彩色机型则为
`machines/ibmtp700c/39G9996.bin`，以及 `video/mda/mda.rom` 等正常字体树。

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
该启动器及介质工具专用于单色 700，不要对 700C 介质使用这一修复脚本。

## 控制方式

- 点击客机画面捕获鼠标。
- `Ctrl+End` 释放鼠标；鼠标中键是备用方式。
- `Ctrl+Alt+Page Up` 切换 SDL 前端全屏。
- 使用窗口关闭按钮前，建议先退出全屏。

模拟器获得焦点后，Qt 软件渲染和 SDL 前端还支持以下 LCD 快捷键：

| 快捷键 | LCD 调节 |
| --- | --- |
| Ctrl+Shift+F5 / F6 | 降低 / 提高亮度 |
| Ctrl+Shift+F7 / F8 | 降低 / 提高对比度 |
| Ctrl+Shift+F9 | 恢复原始颜色 |

这些调节只改变客机 RGB 输出，画面不增加控件、数值或叠加提示，也不修改宿主屏幕。
功能键由模拟器处理；正常退出时保存设置，默认亮度与对比度均为 100。
本配置请使用 Qt 软件渲染，其他 Qt 渲染器尚未验证这些控制。

## 后续开发约定

今后的源码开发统一在本仓库进行。构建目录、ROM、虚拟机状态、磁盘镜像、日志和批量
抓帧应放在被忽略的目录。提交前至少执行：

```bash
git diff --check
cmake --build <你的构建目录> -j2
```

提交说明应指出对应的固件路径或硬件行为；验证材料应使用可复现日志或少量精选截图，
不要提交连续原始抓帧。

英文和中文 README 应同步更新。公开文件检查和 DMA 回归命令见
[验证记录](docs/VERIFICATION.md)。已有本地 700C 运行环境时，可用
[`scripts/package-private-thinkpad700c.ps1`](scripts/package-private-thinkpad700c.ps1)
生成带 README、源码快照和校验文件的私人交接包。输出保存在被忽略的
`private-dist/` 中，不上传到 GitHub。

## 许可证与来源

本分支保留 86Box 的 GNU GPL v2-or-later 许可证和上游声明，详见
[`COPYING`](COPYING) 与 [`AUTHORS`](AUTHORS)。除非相关文件另有明确说明，对本仓库的
新贡献采用相同许可证。

上游参考资料：

- [86Box 项目](https://github.com/86Box/86Box)
- [官方构建指南](https://86box.readthedocs.io/en/stable/dev/buildguide.html)
- [ROM 搜索路径](https://86box.readthedocs.io/en/latest/usage/roms.html)
