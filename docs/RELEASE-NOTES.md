# Update notes

## 2026-11-13 — ThinkPad 700C color and private handoff

The 700C now has its own machine definition, original F1EC firmware identity,
NVRAM names and color display path. It uses the 700C Reference Diskette rather
than treating the monochrome 700 media as interchangeable.

- Corrected VPD checksums, empty-log initialization and MCA ESDI/PS/2 DMA
  arbitration, including a resumable READ BUFFER transfer.
- Added the 512 KiB VGA256C path and 90FB video POS identity used by IBM's color
  driver. Windows 3.2 standard mode was verified at 640×480 with 8-bit scanout.
- Matched the LCD layout to a fixed 640×480 panel: 400-line text is centered by
  default; optional vertical expansion is a machine setting.
- Added Ctrl+Shift+F5/F6 brightness, F7/F8 contrast and F9 original-color reset.
  No controls or overlays are drawn over the guest display.
- Removed full-RAM IML signature scans from normal CPU execution. Timed POST
  retained the original memory test; the reported CP67 stall was not reproduced.
- Rewrote the synchronized English/Chinese README and added a dated verification
  record, focused DMA regression and public-tree CI check.
- Added explicit-file private packaging with a new Chinese handoff README,
  source ZIP, provenance, checksums, verification and desktop-shortcut helpers.
  Private outputs remain ignored and are not included in GitHub artifacts.

The 700 monochrome Reference Diskette boot regression still passes. Enhanced
Windows mode, extended graphics acceleration and full hardware diagnostics remain
outside the verified scope. See [the test record](VERIFICATION.md).

### 中文更新说明

700C 已改为独立机型，保留 F1EC 原始固件身份、独立 NVRAM 和彩色显卡路径，
启动使用 700C 专用参考盘。此次修正包括 VPD 校验、空错误日志初始化、ESDI DMA
仲裁与读缓冲区续传；Windows 3.2 标准模式实测为 640×480、256 色扫描。

参考盘的 400 行文本默认在 480 行面板中居中，另提供纵向展开选项。
亮度和对比度直接使用 Ctrl+Shift+F5/F6、F7/F8，F9 恢复原始颜色，客机画面无新增
控件或提示。正常 CPU 执行不再进行全内存 IML 诊断扫描；原始内存自检仍保留，
本地未复现 CP67 卡死。

GitHub 中英文 README 已同步重写，并新增验证记录、DMA 回归和公开文件检查。
私人交接包包含全新中文 README、源码快照、校验文件和启动辅助脚本，仅保存在本地。
700 单色启动回归通过；增强模式、扩展视频加速与完整硬件诊断尚未验证。
