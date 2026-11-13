# ThinkPad 700C 私人交接包

生成时间：**@CREATED_UTC@**

源码提交：`@SOURCE_COMMIT@`

源码分支：`@SOURCE_BRANCH@`

项目：[86Box ThinkPad 700 / 700C](https://github.com/Deng-ge-open-source/86Box-Thinkpad700_700C)

这是本次完成的 **700C 彩色全屏版本**，包含本地已有的固件、700C 参考盘、
诊断盘、显卡驱动盘、已配置硬盘及 NVRAM。它供你私人使用和后续开发交接，
不要将整个包上传到公开 GitHub。模拟器源码的 GPL 许可证不授予 IBM 介质的再分发权。

## 先启动

1. 将 ZIP **完整解压**到新的固定目录，不要直接在压缩包中运行。
2. 双击根目录的 **`Start-ThinkPad-700C.bat`**。
3. 模拟器默认全屏，保持 4:3。冷启动先进入 **700C Reference Diskette 1.20**。
4. 在参考盘选择 **Start operating system**，确认后启动硬盘上的中文 Windows 3.2。

无需重新安装原包中的 Windows 或驱动。首次运行前可执行文件校验，见下文。
不要将本包的可写硬盘/NVRAM 同时挂载到另一个运行中的模拟器。

开机显示递增内存计数和 CP 检查点属于原始固件行为。16 MiB 配置实测完整 POST
约 38 秒，其中内存检查约 32 秒；CP67 在本地计时中立即通过，没有跳过自检。

## 本次 update

- 700C 使用独立 F1EC 机型身份和原始 `39G9996` ROM，使用彩色机型专用参考盘。
- 修复系统身份校验、配置/存储交接及空错误日志初始化；实测参考盘错误日志为空。
- 安装 IBM VGA256C 1.20 专用驱动并在 Windows 前加载 `VESA.COM`。
  Windows 3.2 标准模式实测为 **640×480、8 位扫描、512 KiB 显存**。
- 参考盘 640×400 内容居中，上下各留 40 个原始面板行；Windows 640×480 铺满面板。
- 增加纯键盘亮度和对比度调节，画面不加按钮、滑条、数值或提示。
- 正常运行不再反复扫描全内存查找旧 IML 标识；原始固件测试保留。
- 公共中英文 README、更新说明和验证记录与本次源码同步。

## 快捷键

请先使模拟器窗口获得焦点。

| 按键 | 功能 |
| --- | --- |
| Ctrl+Shift+F5 / F6 | 降低 / 提高亮度 |
| Ctrl+Shift+F7 / F8 | 降低 / 提高对比度 |
| Ctrl+Shift+F9 | 恢复原始颜色，亮度/对比度均为 100 |
| Ctrl+End | 释放鼠标 |
| Ctrl+Alt+Page Up | 默认的全屏切换快捷键 |
| Ctrl+F11 | 默认的渲染画面截图快捷键 |

亮度/对比度只改变模拟器的客机 RGB 输出，不修改宿主屏幕亮度。
按 F9 后整张画面已与初始截图逐像素比对一致。正常退出后调节值随虚拟机配置保存。
本包使用 Qt 软件渲染，不要切换到尚未验证这些功能的其他 Qt 渲染器。

## 建立桌面快捷方式

在解压后的目录打开 PowerShell，执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Create-Desktop-Shortcut.ps1
```

脚本创建名为 **ThinkPad 700C 私人交接版** 的桌面快捷方式，指向当前解压目录。
移动目录后请重新运行脚本。原有“ThinkPad 700C 彩色全屏”快捷方式不会被覆盖。

## 校验与备份

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Verify-Package.ps1
```

默认校验模拟器、文档、源码快照和 ROM 等固定文件。**首次启动前**可加 `-AllFiles`
校验整个交接快照，包括可写硬盘、软盘、NVRAM 和配置。运行后这些可写文件变化是
正常现象，因此默认校验会跳过它们。`SHA256SUMS.txt` 记录打包时全部文件的摘要；
`PACKAGE-MANIFEST.json` 标识可写文件。

本包模拟器 SHA-256：`@EXECUTABLE_SHA256@`。

修改虚拟机前备份 `runtime/media/` 和 `runtime/ThinkPad 700C/`。
恢复初始状态时，将原始 ZIP 重新解压到**另一个新目录**，保留当前工作副本。
退出时先从程序管理器退出 Windows，回到 DOS，再退出模拟器，避免丢失缓存写入。

## 文件说明

```text
README.md                         本次全新的中文使用与交接说明
86Box.exe                         Windows x64 Qt 软件渲染模拟器
Start-ThinkPad-700C.bat            相对路径全屏启动入口
Create-Desktop-Shortcut.ps1        可选桌面快捷方式创建脚本
Verify-Package.ps1                包内固定文件/初始快照校验
BUILD-INFO.json                   源码提交、生成时间、二进制摘要
PACKAGE-MANIFEST.json              文件摘要及可写状态标识
SHA256SUMS.txt                     初始文件快照摘要
COPYING, AUTHORS                   模拟器许可证与致谢
source/86Box-ThinkPad700_700C.zip   对应提交的公开源码快照，无 .git 和私人介质
verification/                     本次验证/更新记录及精选截图
runtime/roms/                     本地固件及字体 ROM
runtime/media/                    参考盘、诊断盘、驱动盘和可写系统硬盘
runtime/ThinkPad 700C/             已验证配置及独立 NVRAM
```

源码 ZIP 内有同步更新的英文 `README.md`、中文 `README.zh-CN.md` 和构建说明。
工具链和历史调试资料未放入本交接包，后续编译按源码 README 的 MSYS2 UCRT64 步骤进行。
本包的文件路径均为可迁移的相对路径，不依赖原开发机用户名。

## 验证范围与后续交接

详见 [验证记录](verification/VERIFICATION.md)与[更新说明](verification/RELEASE-NOTES.md)。
已验证 700C 冷启动、参考盘配置/身份页面、空错误日志、参考盘到硬盘交接、Windows
3.2 标准模式彩色输出、亮度/对比度快捷键和 700 单色启动回归。

Windows 增强模式、扩展显卡加速、完整 IBM 硬件诊断和所有可选外设尚未验证。
本地未复现间歇性的 CP67 卡死，不把这一点当作对所有 VM 状态的保证。

继续开发时请从 `source/` 中对应提交的源码或 GitHub 分支开始，保留 `origin` 为
本分支仓库、`upstream` 为官方 86Box。不要将 `runtime/`、ROM、系统镜像或本 ZIP
加入公开提交。700 和 700C 的参考盘与 NVRAM 不可混用，也不要对本包运行单色机型
的 `patch_tp700_media.py`。

## English quick start

Extract the entire ZIP, then run `Start-ThinkPad-700C.bat`. Cold boot enters the
700C Reference Diskette; choose **Start operating system** for Windows 3.2.
Ctrl+Shift+F5/F6 adjusts brightness, F7/F8 contrast and F9 restores original colors.
There are no added controls or overlays on the guest display. This private bundle
contains locally supplied IBM media; keep it off public repositories.
