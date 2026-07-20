# Getting the Miyoo Mini (MStar SSD202D) SD card working in QEMU

The SD host is the **FCIE** controller (`hw/sd/mstar_fcie.c`, at `MSTAR_SDIO_BASE`).
A card attaches via `-drive if=sd,format=raw,file=<img>`; the `miyoomini` machine
already wires `TYPE_SD_CARD` onto the sd-bus. The vendor driver is
`drivers/sstar/sdmmc/` (`ms_sdmmc_lnx.c` + `hal_sdmmc_v5.c`), using the FCIE
MIE-event interface.

Four separate things each had to be right for detection **and** access to work.
These are the usual failure points; check them in this order.

## 1. Card-detect (CDZ) is on the PM-GPIO bank, active-low

Modelled in `hw/gpio/mstar_pm_gpio.c` at `0x1f001e00`. The vendor reads CDZ at
PM_GPIO_BANK reg `0x47` (= byte offset `0x11c`), **bit2**:

* card present = bit2 **0**
* empty       = bit2 **1**

Tie it to `drive_get(IF_SD)`. If this bank accidentally reads 0 it looks
"present"; if bit2 is 1 the driver issues **zero** commands.
`GET_CARD_REG_ADDR(bank,idx) = bank + (idx<<2)`.

## 2. SD_STS error bits are write-1-to-clear

The SD_STS error bits (bits 0–5) are **W1C**, not a plain store. The vendor
`_REG_ClearSDSTS` writes the error bits to clear them, then treats any non-zero
read-back as `EV_STS_RIU_ERR` (0x40). If the model just latches the written
value, **every command fails at CMD0**. After fixing, CMD0 / CMD8
(`0x1aa` → SD2.0) / CMD55 (resp `0x120`) return correct responses.

## 3. The big one: the response FIFO must allow byte reads

The vendor `Hal_SDMMC_GetRspToken` reads the command response **one byte at a
time** via `CARD_REG_L8/H8` (U8 reads at the CFIFO position `bank0+0x80` =
`0x1f282080/84/88`). If the FCIE MemoryRegion has `valid.min_access_size = 2`,
QEMU **silently drops every byte read** (the handler is never called) → the
driver sees all-zero responses → enumeration collapses at CMD55/op-cond.
Command *writes* are 16-bit `CARD_REG`, so they work — which is why only
responses fail, a confusing symptom. (Same class of bug as the mstar i2c
`min_access_size` issue.)

Fix: set `valid.min_access_size = 1` and make the read handler byte-aware. The
FIFO is 16-bit words at a 4-byte stride, each word = 2 consecutive response
bytes, so return `(word >> (8*byteoff)) & (size == 1 ? 0xff : 0xffff)` for both
the FIFO and the control regs.

## 4. Synthesize a benign R1b for the vendor's explicit CMD12

The model auto-stops multi-block transfers; the vendor then sends its *own*
CMD12, which otherwise returns NORSP (error `0x08`). Return a benign R1b
(status `0x900`) for that case.

## You also need a real FAT image

The sandbox has no `mkfs.vfat`. Build a FAT16 image (MBR partition type `0x0e`,
FAT16 LBA); the project has `scripts/mstar/`-style helpers for this (e.g. the
`miyoo-mkfat.py` used to produce `miyoo-sdcard.img`). Watch bytearray
slice-assignment when hand-building: `disk[a:a+len(x)] = x`, not
`disk[a:(a+len)*SEC]`, or the image gets resized.

## Verifying

Two ways:

* **Mainline 6.5 kernel** (proves the model in isolation):
  ```
  qemu-system-arm -M miyoomini -kernel <zImage> \
      -dtb mstar-infinity2m-ssd202d-miyoo-mini.dtb \
      -drive if=sd,format=raw,file=<img> \
      -append "console=ttyS0,115200"
  ```
  Expect `mmc0: new high speed SD card`, `mmcblk0: 64.0 MiB`, `mmcblk0: p1`.
  (The 6.5 kernel needs the MIU DDR-PLL fake, or it divides-by-zero first.)
* **Vendor firmware**: boot to MainUI and confirm `/dev/mmcblk0p1` mounts on
  `/mnt/SDCARD` (vfat).

Both should enumerate and read the partition.

## History / caveat

The fixes landed across commits `66462e56fc` (SD_STS W1C), `a45cff16ee`
(PM-gpio CDZ) and `0973e0ccfa` (FIFO byte-access + CMD12). The tree has been
reworked since, so grep the current source for `min_access_size`, `SD_STS`, and
the CDZ `0x11c` / bit2 read rather than trusting old line numbers — a regression
during a reorg is a plausible reason SD "stops working."
