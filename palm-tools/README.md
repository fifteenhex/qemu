# Palm machine test tooling

Helpers for bringing up and testing the Palm machines (`palmv`,
`palmiiix`, `palmvx`, `palmm100` on the MC68EZ328; `palmm500` on the
MC68VZ328).  The full design and RE journal is in `../PALM-NOTES.md`;
this is the how-to-test crib.

## ROMs

Not in git (see PALM-NOTES.md "ROMs").  Fetched into
`/workspace/src/palm-roms/` from archive.org item
`20250707_20250707_0134`:

    Palm-V-3.1-en.rom      (Palm V,  PalmOS 3.1)    md5 c575ebb95f736e389d9c29ad919b4753
    Palm-IIIx-3.1.rom      (IIIx,    PalmOS 3.1)    md5 1022a3ecca4e18e212956a4f5cb79fb4
    Palm-Vx-4.1-en.rom     (Vx,      PalmOS 4.1)    md5 e56adbdffb6420725b0dc5b6fa95b36c
    Palm-m100-3.51-en.rom  (m100,    PalmOS 3.5.1)  md5 d5eaa0eb27e1ae35b33f04dd7b762ad6
    Palm-m500-4.1-en.rom   (m500,    PalmOS 4.1)    md5 dc8f0f8a6ffed58764065a7abe468ce4

(`Palm-IIIx-4.0.rom` in the item is a truncated HTML page, not a ROM.)

After the digitizer is calibrated, the Setup wizard's buttons sit at
the bottom of the form: Previous (20,152), Next (62,152), Done
(97,152) in screen pixels — multiply by 204.8 for `tap:` coords.
Idle machines auto-off after ~2 minutes and then ignore pen taps;
send `key:f5` (power) to wake them first.

POSE-derived reference sources live in `/workspace/src/pose-ref/`
(from github.com/cloudpilot-emu/cloudpilot-emu).

## Build

    cd /workspace/src/qemu-palm && mkdir -p build && cd build
    ../configure --target-list=m68k-softmmu --disable-docs --disable-werror
    ninja qemu-system-m68k

## Run interactively

    ./qemu-system-m68k -M palmv -bios /workspace/src/palm-roms/Palm-V-3.1-en.rom \
        -display gtk,zoom-to-fit=on

or headless with a QMP socket for scripting:

    ./qemu-system-m68k -M palmm500 -bios /workspace/src/palm-roms/Palm-m500-4.1-en.rom \
        -display none -serial null -qmp unix:/tmp/palm.qmp,server,nowait &

## palmctl.py — drive the UI over QMP

    palm-tools/palmctl.py /tmp/palm.qmp tap:16384,16384 sleep:2 dump:/tmp/s.ppm
    palm-tools/palmctl.py /tmp/palm.qmp key:f1          # Date Book hard button
    palm-tools/palmctl.py /tmp/palm.qmp key:f9          # Calculator silkscreen

Pen taps use absolute 0..32767 coords; `key:` uses QEMU qcodes.  See
the script header for the full host-key -> Palm-button map.

Heads-up: the m500 digitizer calibration during Setup is sensitive to
exact tap coordinates and is fiddly to script; the Palm V calibrates
more forgivingly.

## qtest-devices.py — deterministic device tests

No guest code or UI needed; pokes the peripherals directly and checks
the result.  Covers PWM tone synthesis and the RTC watchdog.

    palm-tools/qtest-devices.py ./build/qemu-system-m68k \
        /workspace/src/palm-roms/Palm-V-3.1-en.rom

(The LCDC gray palette is a visual feature — screendump under qtest
doesn't see framebuffer writes, so verify it by eye on the m500.)

## SD card (m500)

    palm-tools/make-sd.sh /tmp/palm-sd.img 64
    ./qemu-system-m68k -M palmm500 -bios .../Palm-m500-4.1-en.rom \
        -drive if=sd,format=raw,file=/tmp/palm-sd.img -display none -serial null

To confirm PalmOS mounts it without navigating the UI, uncomment
`#define DEBUG_SSI_SD 1` at the top of `hw/sd/ssi-sd.c`, rebuild, and
watch the command trace on stderr — a mounted FAT16 volume shows the
init sequence followed by many `CMD17` (read-block) lines.

## Audio

PWM sound needs the audiodev wired to the machine (not just declared):

    -machine palmv,audiodev=snd0 -audiodev pa,id=snd0     # or wav/oss/...

PalmOS only drives the speaker for alarms / SndDoCmd tones, not UI
clicks, so you won't hear taps.
