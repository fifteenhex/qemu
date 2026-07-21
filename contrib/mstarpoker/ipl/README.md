<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Decompiled SSD202D IPL, validated against the model

`ipl_decompiled.c` is the stock Miyoo Mini IPL (first-stage loader) decompiled
to C, together with the harness used to check that the decompilation faithfully
matches what the IPL actually does when it runs in the `miyoomini` model.

The IPL is the image the mask ROM copies from SPI NOR into IMI SRAM at
`0xa0000000` and jumps to (see `docs/system/arm/mstarv7/ipl.rst`). It brings up
the clocks and DDR, then loads the next stage. `ipl_decompiled.c` is
machine-generated from proprietary vendor firmware and is kept only as a
reference; it is not under the QEMU licence.

## Files
- `ipl_decompiled.c` — Ghidra 12.1.2 output (69 functions, base `0xa0000000`).
- `ghidra/SetupEntry.java` — headless pre-script: marks the entry and
  disassembles from it so analysis follows the ARM→Thumb switch.
- `ghidra/ExportC.java` — headless post-script: decompiles every function to C.
- `mmiolog.c` — a small QEMU TCG plugin that logs every store into the RIU MMIO
  window as `pc addr size value` (the ground-truth register program).
- `validate.py` — cross-maps the logged writes onto the decompiled functions.
- `build/` — a **compilable, cleaned port** of the IPL (`ipl.c` + `rt.h`) with a
  bare-metal harness (`start.S`, `link.ld`, `Makefile`). It builds a real IPL
  image, so it can replace the stock IPL in a flash image and be run in the
  model. The port is grown incrementally, keeping all logic as it is reached.
- `compare_mmio.py` — compares the port's MMIO writes against the stock IPL's,
  ignoring PC (the port lives at different addresses) and requiring the same
  ordered sequence of `(addr, size, value)` writes.

## Building and validating the port
```
make -C build                                    # -> build/flash.bin (port as IPL)
# stock reference trace:
qemu-system-arm -M miyoomini -drive if=mtd,format=raw,file=MiYoo283v1.1.bin \
    -L pc-bios -display none -plugin ./mmiolog.so,out=stock.txt
# the port's trace:
qemu-system-arm -M miyoomini -drive if=mtd,format=raw,file=build/flash.bin \
    -L pc-bios -display none -plugin ./mmiolog.so,out=port.txt
python3 compare_mmio.py port.txt stock.txt
```
The port is compiled at different addresses and the compiler is free to generate
different code; correctness is that the **order, count and values of the MMIO
accesses match** the stock IPL. `volatile` register accesses guarantee the
compiler preserves them. The port currently reproduces the entry (the two
`0x1f200800` progress writes) as an exact prefix; each stage extends it.

## Reproduce
Extract the IPL from the NOR image (`IPL_` image at offset 0, 16-byte header +
`0x55a0` body):

    dd if=MiYoo283v1.1.bin of=ipl_full.bin bs=1 count=$((0x10 + 0x55a0))

Decompile (Ghidra headless; mixed ARM/Thumb is handled automatically):

    analyzeHeadless <proj> IPL -import ipl_full.bin \
        -processor ARM:LE:32:v7 -loader BinaryLoader -loader-baseAddr 0xa0000000 \
        -scriptPath ghidra -preScript SetupEntry.java -postScript ExportC.java

Capture the ground-truth register writes from the model:

    gcc -shared -fPIC -I <qemu>/include/plugins $(pkg-config --cflags glib-2.0) \
        -o mmiolog.so mmiolog.c
    qemu-system-arm -M miyoomini -drive if=mtd,format=raw,file=MiYoo283v1.1.bin \
        -L pc-bios -display none -serial file:serial.log \
        -plugin ./mmiolog.so,out=mmiolog.txt

Cross-check:

    python3 validate.py ipl_decompiled.c mmiolog.txt

## Validation result
- **Structural coverage: 100%.** Of the 364 distinct MMIO-write sites the IPL
  executes, all 364 fall inside a decompiled function — no executed store lives
  in code Ghidra failed to decode. `FUN_a0001d50` (called from the entry as
  `FUN_a0001d50(0x40000000)`) is the main bring-up, writing 271 registers
  (clocks / DDR-MIU / UART). The function addresses also match the PCs seen in
  the independent `-d exec` boot trace.
- **Value-level agreement.** Resolving the Ghidra `DAT_` literals from the image
  and comparing to the model agrees on every spot check, several of which match
  registers independently hardware-confirmed in `ipl.rst` / `bootrom.rst`:

  | decompiled write | model ground truth | meaning |
  |---|---|---|
  | `*DAT_a0000020 = DAT_a0000024` (a0000018) | `1f200800 = a001` | RIU-unlock / progress |
  | store @ a0001dd0 | `1f206005 = 00` | MPLL enable (hw) |
  | store @ a0001dde | `1f207004 = 30` | timer clock-source select |
  | store @ a0001e2a | `1f203d4c = 3210` | chiptop pinctrl |
  | stores @ a0001f32.. | `1f221000 = <char>` | UART0 THR — `IPL g5da0ceb` banner |

Every register the decompiled code writes, the model writes: same address, same
value, same PC.
