# AVAL DATA AVME-352 — how to program it from the VMEbus

Reverse engineered from `avme352-ver1dot3.bin`
(`Copyright AVAL-DATA Corp. 1994 / ROM Type = 0001 / AVME-352 Ver 1.3`),
and verified by running that ROM under a purpose-built QEMU machine
(`hw/m68k/avme352.c`, branch `avme352`).

---

## 1. What the card is

A six channel intelligent asynchronous serial I/O card for the VMEbus.
It is a small computer in its own right:

| Part | Where (local 68020 address space) |
|---|---|
| MC68020 CPU | — |
| 512 KiB DRAM | `0x00000000`–`0x0007FFFF` |
| 128 KiB boot ROM (~7 KiB used) | `0x00C00000` |
| Zilog Z8530 SCC ×3 → 6 channels | `0x00F00000`, `0x00F00004`, `0x00F00008` |
| Strap/configuration register (read only) | `0x00F0000D` |
| LED / status latch (6 bits) | `0x00F0000E` |
| Zilog Z8536 CIO, used as a 100 Hz tick | `0x00F0000F` |
| Doorbell interrupt **enable** registers | `0x00F20000`, `+4`, `+8` |
| Doorbell interrupt **acknowledge** registers | `0x00F30000`, `+4`, `+8` |
| **4 KiB dual ported RAM** | `0x00FF0000`–`0x00FF0FFF` |

**The dual ported RAM is the entire host interface.** From the processor
board you never touch an SCC register, never see an interrupt vector from
the SCCs, and never care that there is a 68020 on the card. You write
bytes into a 4 KiB window and read bytes back out.

The card copies the first 64 KiB of ROM down to local RAM at `0x400` and
runs from there, so the ROM listing addresses in this document are RAM
addresses (ROM offset + `0x400`).

> **Note on the ROM image**: the supplied dump has the two bytes of every
> 16-bit word swapped. De-swap it before disassembling. The QEMU model
> detects and corrects this automatically.

### Channel numbering

Channels are numbered 0–5 and map onto the SCCs as:

| Channel | Chip | SCC channel | Control reg | Data reg |
|---|---|---|---|---|
| 0 | SCC 0 | A | `0xF00002` | `0xF00003` |
| 1 | SCC 0 | B | `0xF00000` | `0xF00001` |
| 2 | SCC 1 | A | `0xF00006` | `0xF00007` |
| 3 | SCC 1 | B | `0xF00004` | `0xF00005` |
| 4 | SCC 2 | A | `0xF0000A` | `0xF0000B` |
| 5 | SCC 2 | B | `0xF00008` | `0xF00009` |

(Only relevant if you are debugging the card itself. The host never sees
these.)

---

## 2. The VMEbus view

The card exposes its 4 KiB of dual ported RAM as a VMEbus slave. The base
address and the address modifier (almost certainly A24) are set by
jumpers/switches on the card — the ROM contains no evidence of them
because that decode is pure hardware. **Everything below is an offset
into that 4 KiB window.**

Three locations in the window are *handshake* locations: touching them
from the VMEbus is what wakes the on-card CPU up. This is why the card
does not need to poll:

| Offset | Access from VMEbus | Effect |
|---|---|---|
| `0x0000 + ch` | **read** | "I took that received byte, give me the next one" |
| `0x0008 + ch` | **write** | "transmit this byte" |
| `0x0010 + ch` | **write** | "execute this command" |

Everything else in the window is ordinary shared memory, except the four
bytes at `0x0FFC`–`0x0FFF`, which are the interrupt mailbox.

### Global area, offsets `0x0000`–`0x007F`

| Offset | Size | Direction | Meaning |
|---|---|---|---|
| `0x0000 + ch` | byte | card → host | Fast receive port for channel *ch*. Reading it returns the oldest received byte **and** asks the card to load the next one. When the receive buffer is empty the card leaves the per-channel *fill byte* here (channel block `+0x36`). |
| `0x0008 + ch` | byte | host → card | Fast transmit port. Writing a byte queues it for transmission on channel *ch*. |
| `0x0010 + ch` | byte | host ↔ card | Command / status byte for channel *ch*. See §4. |
| `0x0018`–`0x007F` | | | unused |

### Per-channel blocks

Channel *ch* has a 640-byte (`0x280`) block at

```
    0x0080 + ch * 0x280
```

so: ch0 `0x0080`, ch1 `0x0300`, ch2 `0x0580`, ch3 `0x0800`, ch4 `0x0A80`,
ch5 `0x0D00`. Layout (offsets relative to the block):

| Off | Size | Dir | Meaning |
|---|---|---|---|
| `0x00` | byte | card | channel number + 1 while the channel is open, 0 when closed |
| `0x01` | byte | card | latched receive error: bit0 parity, bit1 overrun, bit2 framing |
| `0x02` | byte | card | current baud index |
| `0x03` | byte | card | current data-bits code |
| `0x04` | byte | card | current stop-bits code |
| `0x05` | byte | card | current parity code |
| `0x06` | byte | card | current flow-control code |
| `0x07` | byte | card | auto-enables (RTS/CTS handshake) active |
| `0x08` | byte | card | outstanding event requests, see §6 |
| `0x09` | byte | card | modem/handshake output state (b0 DTR, b1 RTS, b6/b7 XON-XOFF state) |
| `0x0A` | byte | card | line state (b0 break being sent, b1 break being received) |
| `0x0B` | byte | host | b0/b1 disable the fast receive / fast transmit ports (cmds `0x19`/`0x1A`) |
| `0x0C` | word | card | transmit buffer size, always `0x8000` |
| `0x0E` | word | card | receive buffer size, always `0x8000` |
| `0x10` | word | card | **transmit space free** |
| `0x12` | word | card | **received bytes waiting** |
| `0x14` | word | card | events queued for this channel |
| `0x16` | word | card | events lost through queue overflow (saturates at `0xFFFF`) |
| `0x18` | word | host | command parameter 0 |
| `0x1A` | word/long | host | command parameter 1 |
| `0x1C` | word | host | command parameter 2 |
| `0x1E` | word | host | command parameter 3 |
| `0x20` | word | host | command parameter 4 |
| `0x22` | word | host | command parameter 5 (command `0x50` only) |
| `0x28`–`0x2B` | 4 bytes | host | four "special characters"; receiving one raises event `0x20`–`0x23`. All four zero disables the feature. |
| `0x2C` | word | card | number of times this channel has been opened |
| `0x2E` | long | card | 10 ms ticks since the channel was opened |
| `0x32` | long | card | tick count at which the last receive error happened |
| `0x36` | byte | host | fill byte returned by the fast receive port when no data is available |
| `0x38` | byte | host | match-string length, 1–7 (0 disables) |
| `0x39`–`0x3F` | 7 bytes | host | match string; a complete match raises event `0x24` |
| `0x40` | word | card | current match position |
| `0x42` | word | host | receive-error policy: 0 = halt the receiver on error and wait for command `0x0E`; non-zero = clear the error and carry on |
| `0x50`–`0x27F` | 560 bytes | both | **block data buffer** for commands `0x17` (read) and `0x18` (write) |

The card clears `0x00`–`0x27` on every open, so `0x28` upwards (special
characters, fill byte, match string, error policy) can be set once and
survives open/close.

### Identification and globals, offsets `0x0F80`–`0x0FFF`

| Offset | Size | Meaning |
|---|---|---|
| `0x0F80` | 32 bytes | NUL-padded ASCII ID, `"ROM Type = 0001 AVME-352 Ver 1.3"`. **Written last during boot — use it as the "card is alive" flag.** |
| `0x0FF0` | word | heartbeat divider in 10 ms ticks; the card sets it to 100 (1 Hz), command `0x70` changes it |
| `0x0FF2` | word | copy of the strap register `0xF0000D` — lets the host read the card's DIP switches |
| `0x0FF3` | byte | host-settable event delivery policy: 0 = deliver one event per channel round-robin; non-zero = drain one channel completely before moving on |
| `0x0FF4` | word | total events queued across all six channels |
| `0x0FF6` | long | free-running 10 ms tick counter |
| `0x0FFC` | byte | **interrupt / event flag** (see §6) |
| `0x0FFD` | byte | event code |
| `0x0FFE` | byte | fatal error code (3 = bus error, 4 = unexpected exception, 5 = spurious interrupt). Non-zero means the card has died. |
| `0x0FFF` | byte | channel number + 1 for the pending event, or 0 if strap bit 0 is clear |

`0x0FFC`–`0x0FFF` are deliberately excluded from the card's own RAM test,
because writing `0x0FFC` has the side effect of asserting the VMEbus
interrupt.

---

## 3. Bringing the card up

```
reset the card (VMEbus SYSRESET)
wait for offset 0x0F80 to read "ROM Type = 0001 AVME-352 Ver 1.3"
   (the card runs a full RAM and SCC self-test first; allow a second)
optionally read 0x0FF2 to see how the board is strapped
0x0FFC will be 0 and 0x0FFE will be 0 if the card came up healthy
for each channel you want to use:
    set up the persistent fields (0x36 fill byte, 0x42 error policy,
      0x28.. special characters, 0x38.. match string) if you want them
    issue command 0x01 (open) with the line parameters
```

Nothing else is required. The card arms only the command doorbell at
boot; the fast receive and fast transmit ports are armed by the open.

---

## 4. The command protocol

One command at a time per channel. It is a single byte and a small
parameter block.

1. Write the parameters as big-endian 16-bit words at channel block
   `+0x18`, `+0x1A`, …
2. Write the command code to global offset `0x0010 + ch`.
   *This write is the doorbell* — it interrupts the on-card CPU.
3. The card overwrites `0x0010 + ch` with the completion status:

   | Status | Meaning |
   |---|---|
   | `0x00` | success |
   | `0xF0` | unknown command code |
   | `0xF1` | parameter out of range |
   | `0xF2` | wrong state — channel already open |
   | `0xF3` | wrong state — channel not open |

4. Poll `0x0010 + ch` until it changes, **or** set bit 7 of the command
   code and let the card post event `0x01` when it is done.

Setting bit 7 of the command code (`code | 0x80`) requests a completion
event; the card strips it before dispatching, so `0x81` is "open, and
interrupt me when finished".

> The status is written back into the same byte as the command. Do not
> re-issue a command by writing the same value again without first
> observing the status — and do not treat `0x00` as "idle", it means
> "the last command succeeded".

---

## 5. Command reference

`P0`…`P5` are the parameter words at channel block `+0x18`, `+0x1A`,
`+0x1C`, `+0x1E`, `+0x20`, `+0x22`.

### Configuration

| Code | Name | Parameters |
|---|---|---|
| `0x01` | **Open channel** | `P0` baud index, `P1` data bits, `P2` stop bits, `P3` parity, `P4` flow control |
| `0x50` | **Open channel (explicit)** | as `0x01`, plus `P5` = receive-error policy (stored at block `+0x42`) instead of taking the strapped default |
| `0x02` | **Close channel** | — |
| `0x03` | Set baud rate | `P0` baud index |
| `0x04` | Set character format | `P0` data bits, `P1` stop bits, `P2` parity |
| `0x05` | Set flow control | `P0` flow control code |
| `0x06` | RTS/CTS auto-enables on/off | `P0` non-zero = on |

Parameter encodings:

| Field | Values |
|---|---|
| data bits | 0 = 8, 1 = 7, 2 = 6, 3 = 5 |
| stop bits | 0 = 1, 1 = 1.5, 2 = 2 |
| parity | 0 = none, 1 = odd, 2 = even |
| flow control | 0 = none, 1 = XON/XOFF, 2 = RTS/CTS |

Baud index 0–12. The ROM stores Z8530 time constants, not rates:

| Idx | WR13:WR12 | Divisor | Rate (7.3728 MHz RTxC) |
|---|---|---|---|
| 0 | `0x02FE` | 24576 | 300 |
| 1 | `0x017E` | 12288 | 600 |
| 2 | `0x00BE` | 6144 | 1200 |
| 3 | `0x007E` | 4096 | 1800 |
| 4 | `0x005E` | 3072 | 2400 |
| 5 | `0x003E` | 2048 | 3600 |
| 6 | `0x002E` | 1536 | 4800 |
| 7 | `0x001E` | 1024 | 7200 |
| 8 | `0x0016` | 768 | 9600 |
| 9 | `0x000A` | 384 | 19200 |
| 10 | `0x0006` | 256 | 28800 |
| 11 | `0x0004` | 192 | 38400 |
| 12 | `0x0001` | 96 | 76800 |

The card programs WR11 = `0x56` (BRG drives both Rx and Tx clocks),
WR14 = `0x01` (BRG enabled, clocked from the RTxC pin) and WR4 with the
×16 clock bit, so
`rate = f_RTxC / (32 × (TC + 2))`.
**The divisors are fact; the rates are derived.** The ROM does not record
the oscillator frequency, but the divisor set only produces a full set of
standard rates for 7.3728 MHz, so that is the working assumption. If the
board turns out to have a different crystal, scale the whole column — the
*ratios* are fixed.
(Index 10 is the one oddity: it sets WR14 = `0x03`, i.e. the BRG runs
from PCLK rather than RTxC. Treat 28800 as the least certain entry.)

### Data transfer

| Code | Name | Parameters |
|---|---|---|
| `0x17` | **Read block** | `P0` = byte count, 1…560. Must be ≤ the count in channel block `+0x12`. Data is returned in the block data buffer at `+0x50`. |
| `0x18` | **Write block** | `P0` = byte count, 1…560. Must be ≤ the free space in `+0x10`. Put the data in `+0x50` first. |
| `0x13` | Flush receive buffer | — |
| `0x14` | Flush transmit buffer | — |
| `0x15` | Flush both | — |
| `0x16` | Flush the event queue | — |

Both buffers are 32 KiB (`0x8000`) rings inside the card's own RAM.
`+0x10` (transmit space free) and `+0x12` (bytes received) are maintained
continuously by the card, so a polled driver never needs a command just
to find out whether there is work.

### Modem control and line state

| Code | Name | Parameters |
|---|---|---|
| `0x0F` | Send break on/off | `P0` non-zero = start sending break |
| `0x10` | Resume the peer — send XON, or re-assert RTS | — |
| `0x11` | Set DTR | `P0` non-zero = assert |
| `0x12` | Set RTS | `P0` non-zero = assert (fails with `0xF2` if flow control is RTS/CTS) |

### Event requests

| Code | Name | Parameters |
|---|---|---|
| `0x07` | Interrupt when *N* bytes have been received | `P0` = count, 1…30720; `P1` (long, `+0x1A`) = timeout in 10 ms ticks, 0 = none |
| `0x08` | Interrupt when *N* bytes of transmit space are free | `P0` = count, 1…32768; `P1` (long) = timeout in ticks |
| `0x0D` | Interrupt at a given tick count | `P0` = 1 (absolute) or 2 (relative to now); `P1` (long) = tick value |
| `0x09` | Enable receive-error events (`0x30`) | `P0` non-zero = enable |
| `0x0A` | Enable CTS-change events (`0x10`/`0x11`) | `P0` non-zero = enable |
| `0x0B` | Enable DCD-change events (`0x12`/`0x13`) | `P0` non-zero = enable |
| `0x0C` | Enable break events (`0x14`/`0x15`) | `P0` non-zero = enable |
| `0x0E` | Clear the latched receive error and restart the receiver | — |

Requests `0x07`, `0x08` and `0x0D` are one-shot: the card clears them
when they fire. If the condition is already true when the command is
issued, the card posts the event immediately and returns success.

### Fast-port control and housekeeping

| Code | Name | Parameters |
|---|---|---|
| `0x19` | Enable/disable the fast **receive** port | `P0` non-zero = disable |
| `0x1A` | Enable/disable the fast **transmit** port | `P0` non-zero = disable |
| `0x70` | Set the LED heartbeat period | `P0` = ticks (10 ms each) |
| `0x71` | Write the front-panel LED latch directly (diagnostic) | `P0` = 6-bit pattern |
| `0x72` | Dump the card's internal 256-byte channel control block into the block data buffer at `+0x50` (diagnostic) | — |

Commands `0x03`–`0x1A` all require the channel to be open and return
`0xF3` if it is not.

---

## 6. Interrupts and events

### The mailbox

The card maintains a per-channel event queue (4 KiB each) internally and
delivers one event at a time through the mailbox:

```
card:  waits for offset 0x0FFC to be 0
card:  0x0FFD <- event code
card:  0x0FFF <- channel number + 1  (or 0, see strap bit 0)
card:  0x0FFC <- channel number + 1     <-- this asserts the VMEbus IRQ
host:  reads 0x0FFD / 0x0FFF
host:  0x0FFC <- 0                      <-- acknowledge, releases the IRQ
card:  delivers the next event
```

So the host's interrupt service routine is: read `0x0FFC` (channel + 1),
read `0x0FFD` (event code), write 0 to `0x0FFC`. Nothing else.

`0x0FF4` holds the total number of events still queued, and each
channel's `+0x14` holds its own count, so a polled driver can work
without interrupts at all. `+0x16` counts events dropped because a
channel's queue filled (it holds 4096 events).

Write a non-zero value to `0x0FF3` if you would rather the card drained
one channel's events completely before moving to the next; leave it 0 for
round-robin.

### Event codes

| Code | Meaning |
|---|---|
| `0x01` | Command complete (only when bit 7 of the command was set) |
| `0x02` | Transmit space request satisfied (command `0x08`) |
| `0x03` | Transmit space request timed out |
| `0x04` | Receive count request satisfied (command `0x07`) |
| `0x05` | Receive count request timed out |
| `0x06` | Tick count reached (command `0x0D`) |
| `0x10` / `0x11` | CTS deasserted / asserted |
| `0x12` / `0x13` | DCD deasserted / asserted |
| `0x14` / `0x15` | Break ended / break started |
| `0x20`–`0x23` | Special character 1–4 received |
| `0x24` | Match string matched |
| `0x30` | Receive error — parity, overrun or framing |

The `0x08` byte in the channel block mirrors which requests are still
outstanding: b0 receive count, b1 transmit space, b2 tick, b3 CTS,
b4 DCD, b5 break, b6 receive error.

### Receive errors

By default (channel block `+0x42` = 0) a receive error **stops the
receiver**. The card latches the cause in `+0x01`, stamps `+0x32` with
the tick count, lights that channel's LED and posts event `0x30` if
enabled. The host must issue command `0x0E` to clear it and restart the
receiver. Set `+0x42` non-zero (or use command `0x50`) if you would
rather the card clear the error itself and keep receiving.

---

## 7. The two fast paths

For latency-sensitive or single-character traffic there is a byte-at-a-
time path that costs one VMEbus cycle and no command:

**Receiving.** The card keeps the oldest unread byte at `0x0000 + ch`.
Read it; the read itself tells the card to load the next one. When the
buffer is empty the card writes the channel's fill byte (`+0x36`) there,
so pick a fill byte that cannot occur in your data, or check `+0x12`
first. Disable with command `0x19`.

**Transmitting.** Write a byte to `0x0008 + ch`; the write tells the card
to queue it. If the transmitter is idle it goes straight out. Disable
with command `0x1A`.

For anything bulkier use commands `0x17`/`0x18` and the 560-byte buffer,
which move up to 560 bytes per command.

---

## 8. Board straps (`0xF0000D`, mirrored at DPRAM `0x0FF2`)

| Bit | Effect |
|---|---|
| 0 | 0 = the card always writes 0 to `0x0FFF`; 1 = it writes channel + 1 |
| 1 | Enter the firmware download loader instead of running the ROM (§9) |
| 2 | Run the extended burn-in self-test forever instead of the ROM |
| 6 | Default receive-error policy for command `0x01` (0 = halt receiver, 1 = auto-recover) |

Bit 2 in particular is a manufacturing switch: the card loops testing
local RAM, dual ported RAM and all three SCCs, blinking a heartbeat on
LED bit 5, and stops with a failure code in the LED latch.

The six front-panel LEDs (`0xF0000E`) show, in normal operation, each
channel blinking at the heartbeat rate while open and solid when that
channel has a latched receive error. Failure codes from the self-test
path are `0x01`/`0x02`/`0x04` (SCC 0/1/2 failed), `0x08` (dual ported RAM
failed), `0x10` (local RAM failed) and `0x3F` (unexpected exception).

---

## 9. Firmware download

With strap bit 1 set the ROM runs a loader instead of the serial
firmware, so the host can put its own code on the card. The loader lives
at ROM address `0x00C00626` and, importantly, runs *from ROM* — the
branch to it happens before the ROM shadows itself into RAM at `0x400`,
which is exactly where the downloaded image goes.

### The handshake

One 16-bit big-endian word at DPRAM offset `0x0080`, and a fixed 2 KiB
staging buffer at DPRAM offset `0x0100`:

```
card:  0x0080 <- 'OK' (0x4F4B), then polls it every few cycles
host:  fill DPRAM 0x0100..0x08FF with the next 2 KiB of the image
host:  0x0080 <- 'LD' (0x4C44)
card:  copies 1024 words from 0x0100 to the destination pointer,
       advances the pointer by 2 KiB, writes 'OK' again
  ... repeat ...
host:  0x0080 <- 'GO' (0x474F)
card:  jumps to local address 0x400
```

Details that matter:

* The destination pointer starts at `0x400` and only ever advances, so
  chunks land contiguously. `'GO'` always enters at `0x400` regardless of
  how many chunks were sent.
* Every `'LD'` copies **exactly** 2048 bytes, whatever you actually wrote.
  Pad the last chunk.
* There is no bounds check on the destination pointer. 255 chunks
  (510 KiB) would run off the top of the 512 KiB of RAM.
* No checksum, no error reporting, no way to abort. The card never writes
  anything except `'OK'`.
* Any word that is neither `'LD'` nor `'GO'` makes the card rewrite `'OK'`
  and carry on polling, so a garbled write costs you nothing — but wait
  to see `'OK'` again before staging the next chunk.
* The handshake word sits inside what would be channel 0's block in
  normal operation. Harmless, since the serial firmware is not running.

### State handed to the downloaded code

The loader is reached *after* the ROM's power-on initialisation, so quite
a lot is already set up. At the `jmp $400`:

| | |
|---|---|
| Local RAM `0x400`–`0x7FFFF` | tested and left **zeroed** (the test's last pass writes a pattern, EORs it with itself and checks for zero) |
| Dual ported RAM `0xFF0000`–`0xFF0FFB` | tested and left zeroed; `0x0FFC`/`0x0FFD`/`0x0FFE` explicitly cleared, `0x0FFF` untouched |
| `VBR` | 0. All 256 vectors point at a ROM stub at `0x00C005D6` that writes 3 to DPRAM `0x0FFE`, lights all six LEDs, moves the stack and VBR to `0x200000` and wedges. **Install your own table early.** |
| `SR` | `0x2700` — supervisor, all interrupts masked |
| `CACR` | 1 — instruction cache enabled |
| `A7` | `0x7FFFC`, holding the longword `'LAST'` (`0x4C415354`) |
| SCCs | hard reset (WR9 = `0xC0`), then WR2 = `0x70`/`0x80`/`0x90`, WR1 = 0, WR15 = 0. Nothing else programmed; receivers and transmitters disabled. |
| Z8536 CIO | fully initialised: counter 1 loaded with 20000, mode "continuous", vector `0xA0`, interrupt enabled. **The tick starts firing the moment you lower the interrupt mask.** |
| LEDs (`0xF0000E`) | all off — that is the visible sign that the card is in the loader |
| Doorbell enables | never touched by the loader path, so undefined; disable them (`0xF2000n` ← channel) before enabling interrupts |

If you want the vector layout, interrupt levels and doorbell semantics the
stock firmware uses, they are in §1, §6 and §11 — but nothing obliges
downloaded code to keep them.

---

## 10. A minimal driver, in order

```c
/* card is a pointer to the 4 KiB VMEbus window */

/* --- open channel ch at 9600 8N1, no flow control --- */
u8  *cmd  = card + 0x10 + ch;
u8  *blk  = card + 0x80 + ch * 0x280;

put_be16(blk + 0x18, 8);    /* baud index 8 == 9600 */
put_be16(blk + 0x1A, 0);    /* 8 data bits          */
put_be16(blk + 0x1C, 0);    /* 1 stop bit           */
put_be16(blk + 0x1E, 0);    /* no parity            */
put_be16(blk + 0x20, 0);    /* no flow control      */
*cmd = 0x01;
while ((st = *cmd) == 0x01) cpu_relax();
if (st) error(st);

/* --- transmit --- */
memcpy(blk + 0x50, buf, n);            /* n <= 560              */
put_be16(blk + 0x18, n);
*cmd = 0x18;
while ((st = *cmd) == 0x18) cpu_relax();

/* --- receive --- */
n = get_be16(blk + 0x12);              /* bytes waiting          */
if (n) {
        if (n > 560) n = 560;
        put_be16(blk + 0x18, n);
        *cmd = 0x17;
        while ((st = *cmd) == 0x17) cpu_relax();
        memcpy(buf, blk + 0x50, n);
}

/* --- interrupt service routine --- */
u8 chan  = card[0x0FFC];               /* channel + 1, 0 == none */
u8 event = card[0x0FFD];
card[0x0FFC] = 0;                      /* acknowledge            */
```

A driver that wants to be interrupt-driven rather than polled should
issue command `0x07` (interrupt when *N* bytes received) after each read
and command `0x08` (interrupt when transmit space is free) whenever a
write does not fit, and use event `0x01` for command completion by
OR-ing `0x80` into the command codes above.

---

## 11. Confidence

**Established directly from the ROM and confirmed by running it:** the
memory map; the three-SCC/six-channel structure; the whole dual ported
RAM layout; the command dispatch table and all 30 command codes; the
status codes; the event codes and the mailbox handshake; the baud
*divisor* table; the character-format encodings; the two fast paths; the
self-test and download modes.

**Verified end to end in emulation:** open, set-baud (all 13 indices,
checked against the SCC time constants actually written), block write,
block read, both fast ports, the completion interrupt, the receive
watermark event, and all three error codes — on all six channels
simultaneously.

**Inferred, flagged in place:**

* *Baud rates.* The divisors are certain; the absolute rates depend on
  the RTxC oscillator, which the ROM does not record. 7.3728 MHz is the
  only common crystal that yields a full set of standard rates.
* *The VMEbus base address, address modifier and interrupt level/vector.*
  These are set by hardware the ROM never touches. What the ROM does show
  is that the card asserts its interrupt by writing offset `0x0FFC` and
  releases it when the host writes 0 there, and that strap bit 0 selects
  whether `0x0FFF` carries the channel number.
* *The exact semantics of the doorbell registers.* The firmware writes
  `channel | 0x80` to `0xF2000n` to arm and `channel` to disarm, and the
  bare channel number to `0xF3000n` to acknowledge. That reading is the
  only one consistent with all six call sites (notably: the boot code
  arms only the command doorbell, close disarms the two data doorbells,
  and the command epilogue re-arms unconditionally — which would be an
  infinite loop under a "set request" reading). It is nevertheless an
  inference about glue logic, not something the ROM states.
* *Interrupt priority levels.* IPL 3 for the SCCs, 2 for the two data
  doorbells and 1 for the command doorbell. Derived from the firmware's
  own critical sections (`move.w #$2300,sr` around the transmit priming
  code, `#$2100` in the command epilogue) and confirmed experimentally —
  at any higher SCC level the transmit priming race is visible as a
  duplicated first character.

---

## 12. Reproducing this

```sh
# build (no graphics, no networking, no block layer, m68k only)
../configure --target-list=m68k-softmmu --without-default-features \
             --enable-tcg --disable-docs --disable-tools \
             --disable-guest-agent --disable-install-blobs
ninja qemu-system-m68k

# run the card
qemu-system-m68k -M avme352 -bios avme352-ver1dot3.bin -display none \
    -chardev socket,id=c0,path=/tmp/ch0.sock,server=on,wait=off -serial chardev:c0 \
    ... (six of them) ... \
    -gdb tcp::1234
```

The VMEbus slave window is mapped at `0x10000000` in the emulated address
space (QEMU has no VMEbus). `vmehost.py` in the reverse-engineering
directory drives it over the gdbstub and acts as the processor board.

Useful machine properties, all on `avme352-glue`: `config` (the strap
register), `scc-level`, `db0-level`, `db1-level`, `db2-level`,
`timer-level` and `tick-ns`.
