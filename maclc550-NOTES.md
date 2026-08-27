# Macintosh LC 550 / Color Classic II / Performa 550 — session notes (cc2-finish)

Continuation of `CCLASSIC-NOTES.md`.  Prior sessions got the boot past the
Egret cold-start handshakes and machine-ID/capability config but parked it
diverting into the serial diagnostic console ("MicroBug", 0x408b989a) at the
end of machine-config.  This session root-caused that divert and four more
blockers, advancing the boot all the way through the machine-config POST
(including the onboard-video memory test) and into the post-MMU,
interrupt-driven Egret ADB driver.  Screenshot of the furthest state:
`/tmp/maclc550-furthest.png` (the onboard framebuffer scanning out the raw
video-POST walking test pattern that is still sitting in VRAM — i.e. VRAM is
now correctly mapped and displayed; the OS has not yet set up QuickDraw).

## Root-cause fixes this session (all in hw/m68k/maclc550.c)

1. **ROM self-checksum re-sign (THE config→console divert).**
   The relocation-table patch prior sessions apply at ROM file offset 0x3d42
   (0x00a03d52 → 0x40803d52) changes the ROM image, and the power-on
   self-test at ROM `0x40847a24` sums the ROM as 16-bit words (into a 32-bit
   accumulator) from ROM+4 and compares the result against the stored 32-bit
   checksum longword at ROM+0 (0xede66cbd).  The patch raised the word-sum by
   (0x4080−0x00a0)=0x3fe0, so the self-test failed (`d6`=0xffff), and
   `0x408472d0` diverted via `0x4084a6f6 → 0x40847542 → 0x4084a704 →
   0x408b989a` to the console.  **Fix:** after the relocation patch, bump the
   stored checksum at ROM+0 by the same 0x3fe0 (ROM+0 is read as the compare
   target before the sum loop starts at ROM+4, so it is itself outside the
   summed range).  Confirmed live: computed sum 0xede6ac9d now matches the
   re-signed stored value; `d6`=0 and the boot no longer diverts.

2. **Onboard VRAM base is 0x60B00000, not 0xf9000000 (THE video POST).**
   The 0xf9000000 Valkyrie placement was copied unconfirmed from `lc475.c`
   (a 68040 Quadra ROM).  The Sonora "Eric" ROM instead probes and
   memory-tests its VRAM at physical **0x60B00000**: the kind-7 video
   capability init writes the `'256K'`/`'768?'` size signatures and a
   0x6DB6DB6D walking pattern there, and stores 0x60B00000 as the video
   device-globals base (`a4@(34)`).  With that region unmapped, the bit-18
   video capability's dereference faulted and the machine-config POST bailed
   to the console (`d0`=0x40000 at `0x40847042` → `0x4084a6f6`).  **Fix:**
   `MACLC550_VRAM_BASE = 0x60B00000`; the framebuffer + its aliases are mapped
   there (overlap priority 1) over a low-priority catch-all logging stub
   (`sonora_probe`, 0x60000000+16 MiB) that keeps any still-unmodelled
   register probes in that window from faulting the POST.  Found by mapping
   the catch-all stub first and observing the `'256K'` signature + walking
   pattern writes to 0x60B0xxxx.

3. **Session-framed SET_PRAM byte-handshake reply (Egret PRAM writes).**
   The polled boot driver frames pseudo-commands whose reply it reads back
   over the /XCVR byte-handshake as a formal-session SEND followed by that
   read (ROM 0x408b3a80: release the session, spin at 0x408b3a9c until PB3
   goes low, then clock the reply straight out of SR).  The generic session
   `[01 xx]` handler answered no-response, which never lowers /XCVR, so the
   driver hung.  **Fix:** `maclc550_egret_session_pseudo_reply()` — for
   write/set commands (SET_PRAM 0x0c &c.) build the real reply via the shared
   pseudo builder (SET_PRAM actually writes PRAM now) and drive it out through
   the byte-handshake delivery, closing the formal session.  Plus a teardown
   for the stale, fully-drained delivery when the driver reads fewer bytes
   than staged (its 3-byte `[00 00 00]` status vs our 4-byte header) and then
   opens a fresh send.

4. **Session-framed GET_PRAM returns real data (XPRAM validity).**
   The GET_PRAM driver (ROM 0x408b3b6e) does a receive-turnaround and keeps
   the *last* of four reply bytes; a no-response turnaround left junk there,
   so the ROM read a corrupt XPRAM (the seeded 'NuMc' signature, the ddType at
   0x76/0x77, etc. all came back 0), decided parameter RAM invalid and
   reinitialised it.  **Fix:** stage `[00 00 00 <value>]` so the real byte
   lands in the 4th slot the receive-turnaround delivers.

## Current blocker (this session's stopping point)

Boot reaches the **post-MMU, interrupt-driven Egret ADB driver** (0x408d17xx,
VBR relocated).  It sends an autopoll-parameter upload `[01 08 addrHi addrLo
… 12 bytes]` = **WRITE_MCU to MCU addr 0x0110** over the *formal-session*
framing, then spins at **0x408d180a** on VIA1 IFR bit 2 waiting for a
transaction-complete shift interrupt that never comes.

Both obvious completions REGRESS to a **DOUBLE MMU FAULT** (wild PC, VBR=0):
- routing WRITE_MCU through the no-response receive-turnaround → the ISR
  (0x40814912-family) copies the junk reply through the request block's
  uninitialised data pointer (exactly the hazard the existing
  `egret_process` "generic-ACK vs interrupt-only" comments warn about);
- scheduling a final completion interrupt after the byte-handshake reply
  drains → the ISR fires against no pending request and walks a stale
  pointer.

So the WRITE_MCU here is neither a plain byte-handshake reply nor a plain
receive-turnaround.  **Next step:** determine whether this ISR-driven driver
actually opened a real formal session (in which case the session-close ack
interrupt is what it wants, delivered without re-entering the ISR against a
stale request), or whether the session-open detector is mis-flagging its
byte-handshake framing as a session (in which case it should be routed to the
direct pseudo path `pseudo_try_process` the ISR was designed around).  The
unused `egret_session_pseudo` flag was left in place as the hook for the
former.  Instrument VIA1 IER/ACR and the A092 request block at lowmem 0x1d8 /
0x192 across the WRITE_MCU exchange.

## Build / test commands

    # build dir is on tmpfs; source worktree is /workspace/src/qemu-cclassic
    ninja -C /tmp/cc-build qemu-system-m68k

    /tmp/cc-build/qemu-system-m68k -M maclc550 \
      -bios /workspace/files/mac-roms/maclc550.rom \
      -drive file=/workspace/files/HD0-OpenRetroSCSI-7.5.3.hda,format=raw,if=scsi,bus=0,unit=0 \
      -snapshot -serial null -serial null -display none -icount shift=7 \
      -monitor unix:/tmp/cc.sock,server=on,wait=off \
      -d unimp,guest_errors -D /tmp/maclc550.log
    # gdbstub: add -S -gdb tcp::1234 ; gdb-multiarch: set architecture m68k / set endian big
    # screendump via the monitor: screendump /tmp/shot.ppm  (PPM→PNG with PIL)
