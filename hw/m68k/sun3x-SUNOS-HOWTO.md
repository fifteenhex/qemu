# HOWTO: run the genuine Sun 3/80 boot PROM on QEMU `-M sun3x`

Reproducible recipe for booting the **real Sun-3/80 (sun3x) boot PROM** on the
QEMU `sun3x` machine, as the first stage of bringing up the original vendor OS
(SunOS 4.1.1) for 68030-PMMU validation.  Preservation companion to the
Apollo Domain/OS HOWTO.

## 1. Get the boot PROM (128 KiB)

Source: the Sun3 Archive, `https://www.sun3arc.org/ROMs/3_80/`.  Four revisions
exist; use **3.0.3** (the only rev that boots SCSI even with dead NVRAM).  The
`3.0.x` files are Intel-HEX and must be decoded to raw binary; `2.9.2` is
already raw.

| Rev   | file (…/ROMs/3_80/) | raw size | raw SHA1 |
|-------|---------------------|----------|----------|
| **3.0.3** | sun3_80_v3.0.3.gz | 131072 | `e4be2dcbb29fc5c60ed9d838ab241c634fdd24e5` |
| 3.0.2 | sun3_80_v3.0.2.gz | 131072 | `830187dfe58e65289533717a797d2c42da86ac4e` |
| 3.0   | sun3_80_v3.0.gz   | 131072 | `1e045b6f542aaf7808d6567c28a9e734a8c5d815` |
| 2.9.2 | sun3_80_v2.9.2.gz | 131072 | `7ecd4a0d0988c1d1d53fd79ac16c8456ed73ace1` |

```sh
curl -sLO https://www.sun3arc.org/ROMs/3_80/sun3_80_v3.0.3.gz
gunzip sun3_80_v3.0.3.gz          # -> Intel-HEX
# decode Intel-HEX -> raw 131072-byte binary:
python3 - <<'PY'
d=bytearray(); base=0
for l in open("sun3_80_v3.0.3"):
    l=l.strip()
    if not l.startswith(':'): continue
    b=bytes.fromhex(l[1:]); n=b[0]; a=(b[1]<<8)|b[2]; t=b[3]
    if t==0: 
        p=base+a
        if p+n>len(d): d.extend(b'\xff'*(p+n-len(d)))
        d[p:p+n]=b[4:4+n]
    elif t==2: base=((b[4]<<8)|b[5])<<4
    elif t==4: base=((b[4]<<8)|b[5])<<16
    elif t==1: break
open("sun3_80_v3.0.3.bin","wb").write(d)
PY
sha1sum sun3_80_v3.0.3.bin   # -> e4be2dcbb29fc5c60ed9d838ab241c634fdd24e5
```

## 2. Build QEMU

```sh
git worktree add /path/qemu-sun3x-sunos -b sun3x-sunos amiga   # branch sun3x-sunos
mkdir -p /tmp/sun3x-sunos-build && cd /tmp/sun3x-sunos-build
/path/qemu-sun3x-sunos/configure --target-list=m68k-softmmu --disable-docs
ninja qemu-system-m68k
```

## 3. Run the PROM (SunOS mode is entered by `-bios`)

```sh
/tmp/sun3x-sunos-build/qemu-system-m68k -M sun3x -m 16M \
    -bios /path/sun3_80_v3.0.3.bin \
    -display none -serial mon:stdio          # or -serial tcp:...,server
```

With **no** `-bios` the machine is the unchanged Linux shim
(`-M sun3x -kernel vmlinux -append console=hvc0`), which still boots Linux to a
shell (regression-checked: CPU 68030 / MMU 68030).

## 4. Expected console (milestone 1)

The genuine PROM runs its full power-on self-test — **every subtest passes** —
sizes and initialises RAM, exercising the 68030 on-chip PMMU under real Sun
firmware, on the `ttya` serial port:

```
Sun-3/80 Boot PROM Selftest (Rev. 3.0.3)
 (Press <Esc> to abort tests or <Cntrl-L> for the Loop Menu)
System Enable Register Read Test {pass}      PROM Checksum Test {pass}
TOD [Clock/Calender] Test {pass}
I/O Mapper RAM Write/Write/Read {pass}  ...Address {pass}  ...3-Pattern {pass}
<Memory Size = 0x00000010 MB>
Memory Address Test {pass}
Memory Read/Write Byte Alignment Test {pass x2}
Bus Error Register Test {pass}
Level 1/2/3 Interrupt Test {pass x3}
Parity Memory Test {pass}   Parity Memory Forced Error Test {pass}
```
After `q`/`Esc`: `<Selftest aborted, Initializing the Monitor>` and
`<Initializing Main Memory... 0x00000010 Megabytes Initialized>`.

## 5. Debugging

Unique gdb port **1254**, big-endian:
```sh
qemu-system-m68k ... -gdb tcp::1254 -icount shift=7 -D trace.log -d in_asm,int,mmu
gdb-multiarch -q -ex 'set architecture m68k' -ex 'set endian big' \
              -ex 'target remote :1254'
```
Disassemble the PROM: `m68k-linux-gnu-objdump -D -b binary -m m68k:68030 \
  --adjust-vma=0xfefe0000 sun3_80_v3.0.3.bin`.

## 6. Status / next steps

* **Milestone 1 (DONE, deterministic):** genuine PROM + full self-test +
  memory init + monitor self-relocation + bidirectional ttya console +
  **the interactive `>` monitor prompt, reproducibly (10/10 runs, 0 crashes)**
  — enabled by the merged 68030/68851 PMMU core fixes (PTEST `An` writeback
  etc.) plus a board-level NMI vector-gate.  Run adds `-drive`:
  `... -bios sun3_80_v3.0.3.bin -drive file=disk.qcow2,format=qcow2,if=scsi,bus=0,unit=3`
* **Milestone 2 (in progress):** "si" NCR5380 @ 0x66000000 + DMA/CSR block
  (CSR @ +0x1000, DMA regs, data FIFO @ +0x08 / count @ +0x1c) wired and
  passing the PROM's si register/FIFO self-test; boots to `>` with a disk
  attached.  `b sd(0,30,3)` reaches the sd driver but returns "Device not
  found": the si controller isn't yet registered in the PROM's boot-device
  table (its presence-probe/registration path needs completing).  Remaining
  M2: sd registration -> 5380 SELECT -> si DVMA boot-block read -> miniroot
  install (media in /tmp/sunos-media: miniroot_sun3x/munix_sun3x/munixfs).
* **The monitor clock (how `>` is reached):** the boot countdown advances a
  scratch tick counter (virtual `0xfef720c6`, +10/tick) from a **level-7 (NMI)**
  clock handler `0xfefe04fe` that reads a clock-pending register (virtual
  `0xfef06010`) and acks (virtual `0xfef0b400`).  The monitor runs with the
  68030 MMU on, so those virtuals map (via its page tables, resolved with
  `m68k_cpu_get_phys_addr_debug`) to **physical `0x64000810` (pending)** and
  **`0x61002800` (ack)** — where the clock device is placed.  The level-7 line
  is re-armed as a fresh NMI edge each tick.
* **Known limitation:** reaching `>` is currently **non-deterministic** — the
  handler's counter increment is MMU-context-sensitive.  Making it reliable
  needs the counter/handshake driven on the correct side of the MMU in every
  context the NMI handler runs in.  See `sun3x-SUNOS-NOTES.md`.
* **Then Milestone 2:** NCR5380 "si" @ `0x66000000` + DMA `0x66001000` (reuse
  `hw/scsi/ncr5380.c`), attach a SunOS 4.1.1 sun3x disk (qcow2), `b sd(0,30,3)`
  → SunOS kernel → single-user shell on ttya.

See `sun3x-SUNOS-NOTES.md` for the full hardware contract and RE details.

## Milestone 2 — si SCSI + DVMA validated end to end (2026-08)

The full si SCSI + DVMA stack now works against the genuine PROM.  With a
disk attached:

    qemu-system-m68k -M sun3x -m 16M -bios sun3_80_v3.0.3.bin \
      -drive file=disk.qcow2,format=qcow2,if=scsi,bus=0,unit=3 \
      -display none -serial mon:stdio
    > b sd(0,30,3)

drives, in order: bus arbitration/selection of target 3, the CDB, TEST-UNIT-
READY, START/STOP UNIT, and INQUIRY with a real **data-in DVMA transfer** (the
36-byte INQUIRY response is delivered through the IOMMU page table into main
memory), after which the PROM identifies the disk and reads its label.  On a
blank disk the console reaches:

    Boot: No label found - attempting boot anyway.

i.e. the 68030 PMMU is running the real Sun firmware driving real SCSI DMA.

Implementation (all sun-mode gated; Mac ncr5380 path untouched):
* NCR5380 register map: reg0(data)@0x66000008, reg-shift 2; reg2 reads return
  the live bus phase and reg3 reads the driver's dispatch code (0x08 active
  REQ / 0x20 idle-advance); CDB staged via reg0 FIFO and delivered on select;
  ICR bit7 is a DMA strobe, not a bus reset; selection to target = MR&7.
* si glue: DMA count latches at 0x66000000/0x04, si_csr progress/SBC_IP/DMA_IP
  sequencing, DVMA IOMMU translator (sun3x_dvma_to_phys) + pseudo-DMA engine
  (sun3x_si_dma_run) moving 5380 data <-> DVMA memory.

### Remaining to a SunOS shell: a bootable disk
The emulation is proven; the blank qcow2 is the only blocker.  Need a disk
with a Sun disklabel (block 0), the SunOS bootblk, and a miniroot/root fs so
`b sd(0,30,3)` finds a boot program.  Media in /tmp/sunos-media:
munix_sun3x (kernel, m68k a.out OMAGIC), miniroot_sun3x / munixfs_sun3x
(install fs images).  Path: build/obtain a labelled bootable SunOS 4.1.1
sun3x disk (disklabel + installboot bootblk + miniroot), then boot -> install
-> shell on ttya.

## Boot disk construction (sun3x-SUNOS-mkdisk.py) + current blocker

`miniroot_sun3x` is a ready SunOS UFS partition image: block 0 is the
disklabel slot, blocks 1-15 hold the sun3 ufsboot boot block (block 1 begins
with the m68k jmp `0x4efa010a`), UFS superblock at byte 8192.  Build a bootable
disk by laying it at LBA 0 and stamping a Sun disklabel over block 0:

    python3 hw/m68k/sun3x-SUNOS-mkdisk.py /tmp/sunos-media/miniroot_sun3x \
        /tmp/sunos-boot.img          # 256 MiB raw, 1024c/16h/32s, magic 0xDABE
    qemu-system-m68k -M sun3x -m 16M -bios sun3_80_v3.0.3.bin \
        -drive file=/tmp/sunos-boot.img,format=raw,if=scsi,bus=0,unit=3 \
        -display none -serial mon:stdio
    > b sd(0,30,3)

The disklabel is valid (magic 0xDABE @508, checksum xor==0, partitions a/b/c/d
at cyl 0 covering the disk).

**Current blocker (boot-program RE, emulation is proven):** the PROM boot
program runs its SCSI *probe* — TEST-UNIT-READY, START/STOP, INQUIRY (INQUIRY
data delivered via PIO reg0 reads) — all succeed against target 3, but it then
**never issues a disk READ (0x08)**: exactly 3 selections / 3 commands / 0
timeouts, no data-read command.  So it never reads block 0, prints
`Boot: No label found - attempting boot anyway.` and then `sd: Device not
found.`  The block-read path (0xfeff153a -> 0xfeff1550 CDB builder cmd=8 ->
0xfeff0c00) is not reaching the selection/CDB stage after the probe — likely
the standalone disk-read routine aborts before issuing the SCSI command
(geometry/READ-CAPACITY expectation, DVMA-buffer/IOMMU-map setup for the first
DMA read, or an INQUIRY-response field it rejects).  Needs RE of the boot
program's post-probe read path.  Everything up to and including the probe +
data delivery is validated.
