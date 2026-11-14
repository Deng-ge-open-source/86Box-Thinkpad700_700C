# ThinkPad 700 / 700C verification

Last local verification: **2026-11-14 (UTC)**. This records observed results,
not certification of every firmware function or host platform.

For the 2026-11-14 publication, the Windows Qt target built successfully and
the public-tree, DMA, Windows host-floppy cache and HDD-audio scheduling checks
passed again. A separate 25 MHz pure-DOS session also reached the FS 5.1 cockpit.
Earlier dated measurements below retain their original test dates.

## Test configuration

- Windows x64; MSYS2 UCRT64 GCC 16.2, Qt 5.15 software renderer and SDL 2.32.
- Release builds, dynamic recompilation enabled, `NEW_DYNAREC=OFF`.
- IBM 486SLC/25, 16 MiB RAM, integrated MCA ESDI, PS/2 mouse.
- Working disk: 370 cylinders, 64 heads, 32 sectors.
- 700C: original 256 KiB `39G9996`, F1EC identity, separate NVRAM,
  Reference Diskette 1.20 (`r700c120`).
- Windows 3.2 standard mode, IBM VGA256C driver 1.20 and `VESA.COM`.
- All firmware, guest media, full logs and VM state remain local.

## Observed results

| Check | Result |
| --- | --- |
| 700C cold boot | Reached the 700C Reference Diskette main menu before the hard disk |
| Configuration | Automatic configuration, revision and system identity pages usable |
| Error log | More utilities → Display system error log reported empty |
| Hard-disk hand-off | Start operating system completed POST and entered Windows 3.2 |
| Windows color | 640×480, 8-bit scanout, 524,288-byte VRAM, packed chain-4 layout |
| 256-color image | Paintbrush displayed the complete generated bitmap; 245 distinct output RGB colors after Windows palette mapping |
| Text proportions | SDL mapped 640×400 to `(0,40) 640×400` on a 640×480 panel |
| Expanded text option | SDL mapped to `(0,0) 640×480` when expansion was enabled |
| Graphics proportions | True 640×480 graphics used the full panel |
| Qt fullscreen | Rendered capture was 2160×1620; 4:3 panel and centered text retained |
| Brightness keys | Ctrl+Shift+F5 changed gray RGB 170 to 153; F6 returned to the original frame |
| Contrast keys | Ctrl+Shift+F7/F8 changed that gray to 165/174 respectively |
| Reset key | Ctrl+Shift+F9 restored every RGB pixel to the original screenshot |
| Guest input isolation | LCD shortcuts left the Reference Diskette menu unchanged; no overlay was added |
| 700 regression | Same SDL build booted the monochrome Reference Diskette main menu |
| Source checks | Relevant Qt/SDL builds and `git diff --check` passed |

Selected captures: [Reference proportions](images/thinkpad700c-reference-proportions.png),
[empty error log](images/thinkpad700c-error-log-empty.png),
[Windows color](images/thinkpad700c-windows-color.png),
[256-color image](images/thinkpad700c-256-color-test.png).

### POST timing

Checkpoint transitions were timed at firmware port `0680h`, at normal emulator
speed with the same 16 MiB VM. The original and corrected builds both took about
38 seconds through POST; CP52 memory checking accounted for about 32 seconds.
CP67 → CP68 took less than the millisecond timer resolution in both runs.

The reported CP67 hang was not reproduced in this test. A separate diagnostic
loop did repeatedly scan all RAM for an old 700 IML signature on the 700C;
normal builds now exclude that scan. No firmware memory test was skipped, and
this change is not claimed to shorten the original 32-second memory check.

## Windows physical USB floppy — 2026-11-13

The Windows Qt build was tested with a TEAC USB UF000x drive assigned to `A:`
and the user's physical 700C Reference Diskette. Windows reported 80 cylinders,
two heads, 18 sectors per track and 512-byte sectors. Read-only inspection found
`PF1EC.ADF`, `SF1EC.ADF` and `$0009000.IML` on the disk.

With `fdd_01_fn = ioctl://\\.\A:`, the original F1EC ROM completed POST and
booted the physical Reference Diskette to its main menu. The log recorded reads
from the Windows device across multiple cylinders, exceeding 800 KiB, rather
than loading the bundled floppy image. The selection was retained in the saved
configuration. [Captured main menu](images/thinkpad700c-usb-floppy-reference.png).

A focused hardware probe checked boot/root directory reads, out-of-range CHS
rejection, rejection of the fixed `C:` volume, forced guest write protection and
unchanged boot-sector bytes after a rejected write. The backend opens the host
device with `GENERIC_READ` only and contains no physical write operation.
The monochrome 700 image-backed regression also reached its known Reference
Diskette idle firmware trace with the expected F1EE identity.

The private runtime uses the existing maximum sound gain of +18 dB, with the
original drive-profile levels. WASAPI loopback measured hard-disk output peak
0.1170 versus 0.01473 at 0 dB (about 7.94 times the amplitude). The host monitor
and Windows master volume were not changed.

Only a 1.44 MB physical Reference Diskette and this USB drive were tested.
720 KiB/2.88 MB physical media, a separate physical DOS startup disk, media
removal while reading, physical formatting and the complete diagnostic suite
remain unverified. Physical writes/formatting are intentionally unsupported.

## Host-floppy/cache follow-up — 2026-11-13

The TEAC read-only probe was repeated after expanding the cache from one
head/track to previously visited tracks. Five reads of the same cylinder took
3,875 ms / ten physical reads / 92,160 bytes before, versus 750 ms / two reads /
18,432 bytes after. This measures repeated-track access, not whole-boot speed.
Controller seek-completion timing is retained: removing that delay caused a
Reference boot regression during development and was not retained.

The Windows storage-driver mock regression checks track revisits, cache
invalidation after a reported media change or removal, unbuffered sector reads,
CHS bounds, fixed-volume rejection and read-only access without opening hardware.
The real A: boot/root/bounds/write-protection probe also passed again.

With a configured but nonexistent host drive, the original 700C completed POST
and the post-POST INT 19h handoff entered the hard-disk image's color Windows 3.2.
The host path remained configured for the next cold boot. This exercises mount
failure without removing the user's real disk; physical removal during a read
was tested only in the mock, not on the TEAC.

The game installed on the user's HDD identifies itself as Flight Simulator
5.1, despite the initial report of 5.0. A separate working copy booted pure DOS
and entered its color cockpit at IBM 486SLC/25; the emulator indicated 100%
speed. This is not an in-game frame-rate measurement or a comparison against
the same scene on a physical 700C. Game settings and the user's HDD were not
modified for this test.

Live CPU inspection found POR.CE enabled while prefetch timing remained six
cycles and legacy recompilation was blocked by CR0.CD. CR0 mode switches could
also clear the emulator's cache-enable state despite POR remaining enabled.
After correction, the DOS game retained cache enable with zero prefetch penalty.
The banked E000/F000 ThinkPad firmware stays interpreted so its per-instruction
controller and wait-loop hooks are preserved. Cached DOS/Windows RAM code
remains eligible for recompilation.
Windows 3.2 color startup also passed. A separate Windows-standard-mode game
autostart test reached the game splash but did not reliably enter the cockpit;
it is not recorded as passing. The current CONFIG.SYS has no EMS manager and
AUTOEXEC uses WIN /S. Microsoft's original
[FS 5.1 notes](https://jeffpar.github.io/kbarchive/kb/132/Q132944/)
require EMS for running inside Windows and recommend pure DOS for smoother play.

Fresh-state testing reproduced a CP67 delay of about 64 seconds. The integrated
700C ESDI controller powered up at arbitration 5 while POST configured DMA channel 0
for arbitration E, leaving WRITE BUFFER waiting for a channel until timeout.
The matching IBM @DF9F configuration file recommends E; the controller now
powers up at E on the 700C, while the monochrome default remains 5 and later
POS writes can still select other levels. In the corrected 700C run, CP67 to
CP68 took 144 ms. The physical Reference Diskette reached its main menu
without the 00174 warning seen with the mismatched default; the monochrome
700 also reached its own main menu.

The final Qt Release build, DMA regression, Windows host-floppy mock and
public-tree checks passed. The monochrome 700 image-backed boot also reached
the known Reference Diskette idle trace with its F1EE identity.

## HDD audio during a host read — 2026-11-13

The HDD mixer had its own thread but waited indefinitely for emulation-timer
events. A synchronous USB read prevented those events from being produced;
the 80 ms output queue could empty while a track read took hundreds of ms.
In Windows host-floppy mode, the existing worker now uses a 20 ms host-clock
deadline, coalesces early guest events and honors VM pause. Image-only mode
keeps its original event-driven scheduling.

A deterministic test runs the real worker with no guest events after 50 ms
and a 400 ms pause during a two-second interval. The old worker produces zero
blocks; the corrected worker produces 79, resumes after pause, and never sends
blocks faster than the configured cadence. Image-only idle still waits for an
event. The test uses no firmware, media or audio device.

A second test cold-booted separate muted VMs from an offline snapshot with the
real TEAC and Reference Diskette, leaving the user's running VM untouched. A
read-only probe sampled the HDD worker's actual PCM buffer every 5 ms for
45 seconds. The old worker had twelve intervals above 100 ms with no new PCM
(longest 1,379 ms); the corrected worker had none (longest observed interval
73 ms). This measures PCM production during real USB I/O, not a separate
perceptual listening test or a guarantee against every host scheduling stall.

## Repeat the source-only checks

From the repository root, with Python 3 and GCC available:

```bash
git diff --check
python3 tools/thinkpad700/check_public_tree.py
mkdir -p build/thinkpad-tests
gcc -O2 -fwhole-program -Isrc/include -Isrc/cpu -ffunction-sections -fdata-sections \
  tools/thinkpad700/tests/test_dma_arbitration.c -Wl,--gc-sections \
  -o build/thinkpad-tests/dma-arbitration
./build/thinkpad-tests/dma-arbitration
```

On Windows with GCC, also run the hardware-free host-floppy regression:

```powershell
gcc -O2 -Isrc/include tools/thinkpad700/tests/test_win_host_floppy.c -o build/thinkpad-tests/win-host-floppy.exe
./build/thinkpad-tests/win-host-floppy.exe
gcc -O2 -fwhole-program -ffunction-sections -fdata-sections -Isrc/include -Isrc/cpu -Ibuild/windows-700c/src/include tools/thinkpad700/tests/test_hdd_audio_host_wait.c -Wl,--gc-sections -lm -o build/thinkpad-tests/hdd-audio-host-wait.exe
./build/thinkpad-tests/hdd-audio-host-wait.exe
```

The HDD worker test uses the generated headers from the configured build tree;
adjust `build/windows-700c` if your build directory differs.

The DMA test checks arbitration E reassignment, enabled/masked channel selection
and legacy fallback against the actual DMA implementation. These checks need no
firmware. The public-tree check examines files in the Git index, including staged
files; it does not inspect or package ignored local media.

## Repeat the local firmware regression

1. Copy the model's working media and NVRAM into a separate test VM.
2. Build the relevant frontend and cold boot with the correct Reference Diskette
   mounted. Confirm the model and absence of configuration/identity warnings.
3. Open the system error log, then return to the main menu and start the operating
   system. Confirm 640×480 color graphics on the 700C.
4. In the Qt software renderer, capture a stable menu, exercise the five LCD
   shortcuts and compare the reset capture with the original.
5. Boot the monochrome 700 with its own media and NVRAM after shared-core changes.

Use working copies. Preserve a non-empty error log for diagnosis rather than
clearing it to make a test appear successful.

## Not yet verified

- Windows enhanced-mode driver operation and extended video acceleration.
- The complete IBM hardware diagnostic suite and every optional peripheral.
- Cycle-exact timing or calibrated photometric reproduction of an aged TFT.
- This 700C revision on every Linux/macOS/ARM host or every Qt rendering backend.
- Reproduction of the user's intermittent CP67 delay across other VM states.
