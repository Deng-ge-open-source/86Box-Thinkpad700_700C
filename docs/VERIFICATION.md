# ThinkPad 700 / 700C verification

Last local verification: **2026-11-13 (UTC)**. This records observed results,
not certification of every firmware function or host platform.

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
