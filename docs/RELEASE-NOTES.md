# Update notes

## 2026-11-13 — Keep HDD audio playing during USB reads

On Windows, selecting a physical floppy now gives the HDD sound worker a
20 ms host-clock cadence. It no longer depends on emulation-timer notifications
that stop during synchronous USB reads. Early guest events do not accelerate
playback; VM pause and image-only scheduling retain their existing behavior.

Windows 实体软驱模式下，硬盘音效改为按宿主时间每 20 ms 续播，避免 USB 读盘阻塞
模拟时钟时主轴声断续。过早的客机通知不会加快播放；暂停和纯镜像模式保持原行为。

## 2026-11-13 — Host-floppy boot order and SLC cache handling

- Retain host-drive selection when media is absent; after POST, fall back to
  the hard-disk image and retry the physical drive on the next cold start.
- Correct integrated ESDI power-on arbitration to E, matching the 9552 POST
  and IBM's recommended configuration. A level-5 default could leave the
  CP67 buffer test waiting about 64 seconds for DMA.
- Cache previously visited physical tracks, invalidate on reported media
  changes. Controller seek completion timing is retained.
- Suppress sampled floppy sounds automatically while a host drive is selected;
  image-backed floppy profiles and hard-disk sounds remain available.
- Refresh IBM SLC prefetch timing when POR.CE changes. Preserve POR cache
  enable across CR0 writes and use it for legacy recompiler eligibility.
  Intel cache control remains based on CR0.CD. Cache-tag diagnostics and
  banked ThinkPad firmware keep their interpreter hooks.
- Add a Windows storage-driver mock regression covering cached track revisits,
  media replacement/removal, geometry bounds and read-only access.

### 中文更新说明

实体盘缺席时保留软驱选择，自检后回退硬盘镜像，下次冷启动重新探测。实体盘读取会
缓存访问过的磁道，报告换盘时清除；保留控制器寻道完成时序，并自动关闭模拟软驱
音效。硬盘音效和镜像软盘的音效配置保留。

集成 ESDI 控制器的上电仲裁级别改为原配置说明推荐的 E，匹配 9552 自检，修正
默认级别 5 导致 CP67 缓冲区测试等待约 64 秒超时的问题。

修正 IBM SLC 的 POR 缓存开关与预取计时、CR0 模式切换和旧动态重编译器的衔接，
保留缓存诊断使用解释执行。CPU 仍为原配置的 25 MHz。新增无需真实软盘的 Windows
存储驱动模拟回归，覆盖缓存、换盘/移盘、边界及只读访问。

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
