#!/usr/bin/env python3
"""Deterministic device tests for the Palm machines, driven over qtest.

These poke the DragonBall peripherals directly (no guest code) and
check the observable result, so they don't depend on PalmOS timing or
UI navigation.  Run against a build of qemu-system-m68k:

    qtest-devices.py ./qemu-system-m68k /path/to/Palm-V-3.1-en.rom

Covers:
  - PWM 1 tone synthesis (frequency + duty, via the WAV backend)
  - RTC watchdog (interrupt mode fires + services)

The LCDC gray palette is a visual feature and is NOT covered here:
screendump under -accel qtest doesn't see the framebuffer writes
(the display's dirty-memory tracking is driven by CPU writes, not
qtest pokes), so it can't be checked deterministically this way.
Verify it by eye instead — the m500 running OS 4.1 drives the panel
at 2bpp and reprograms LGPMR, so shaded content shows real grays.

Each test starts its own qemu with -accel qtest.  A tmp dir holds the
sockets and WAV captures.
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

MMIO_PWM = 0xfffff500
MMIO_RTC_WDT = 0xfffffb0a
MMIO_INTC_IPR = 0xfffff310


class QTest:
    def __init__(self, path):
        for _ in range(50):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError('qtest socket never appeared')
        self.f = self.s.makefile('rw')

    def cmd(self, c):
        self.f.write(c + '\n')
        self.f.flush()
        return self.f.readline().strip()

    def wb(self, a, v):
        self.cmd(f'writeb 0x{a:x} 0x{v:x}')

    def ww(self, a, v):
        self.cmd(f'writew 0x{a:x} 0x{v:x}')

    def rw(self, a):
        return int(self.cmd(f'readw 0x{a:x}').split()[1], 16)

    def rl(self, a):
        return int(self.cmd(f'readl 0x{a:x}').split()[1], 16)

    def step(self, ns):
        self.cmd(f'clock_step {ns}')


def launch(qemu, rom, tmp, extra=()):
    qt = os.path.join(tmp, 'qt.sock')
    args = [qemu, '-M', 'palmv,accel=qtest',
            '-qtest', f'unix:{qt},server,nowait',
            '-bios', rom, '-display', 'none', '-serial', 'null', *extra]
    p = subprocess.Popen(args, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    return p, qt


def read_wav(path):
    d = open(path, 'rb').read()
    rate, = struct.unpack('<I', d[24:28])
    ch, = struct.unpack('<H', d[22:24])
    n = (len(d) - 44) // 2
    s = struct.unpack(f'<{n}h', d[44:44 + n * 2])
    return rate, s[::ch]


def measure_tone(mono, rate):
    nz = [i for i, v in enumerate(mono) if abs(v) > 2000]
    if not nz:
        return 0, 0
    seg = mono[nz[0]:nz[0] + int(rate * 0.1)]
    zc = sum(1 for a, b in zip(seg, seg[1:]) if a < 0 <= b)
    duty = sum(1 for v in seg if v > 0) / len(seg)
    return round(zc * rate / len(seg)), duty


def test_pwm(qemu, rom, tmp):
    wav = os.path.join(tmp, 'pwm.wav')
    p, qtsock = launch(qemu, rom, tmp,
                       ['-machine', 'palmv,accel=qtest,audiodev=snd0',
                        '-audiodev', f'wav,id=snd0,path={wav}'])
    try:
        qt = QTest(qtsock)
        qt.step(1_000_000)
        # ~1kHz, 50% duty: 16.58MHz/(32+1)/2/(246+2) = 1013Hz
        qt.wb(MMIO_PWM + 0x4, 0xf6)         # period
        qt.wb(MMIO_PWM + 0x0, 0x20)         # PWMC hi: prescaler 32
        qt.wb(MMIO_PWM + 0x1, 0x10)         # PWMC lo: enable
        qt.wb(MMIO_PWM + 0x3, 0x7b)         # sample: ~50% -> starts output
        for _ in range(20):
            qt.step(50_000_000)
            time.sleep(0.02)
    finally:
        p.kill(); p.wait()
    rate, mono = read_wav(wav)
    freq, duty = measure_tone(mono, rate)
    ok = 950 <= freq <= 1080 and 0.4 <= duty <= 0.6
    print(f'  PWM tone: {freq} Hz, {duty:.0%} duty  [{"ok" if ok else "FAIL"}]')
    return ok


def test_watchdog(qemu, rom, tmp):
    p, qtsock = launch(qemu, rom, tmp)
    try:
        qt = QTest(qtsock)
        qt.step(1_000_000)
        qt.ww(MMIO_RTC_WDT, 0x0003)          # enable + interrupt mode
        qt.step(1_000_000_000)               # count 1
        before = qt.rw(MMIO_RTC_WDT)
        qt.step(1_000_000_000)               # count 2 -> fire
        fired = qt.rw(MMIO_RTC_WDT)
        ipr = qt.rl(MMIO_INTC_IPR)
        qt.ww(MMIO_RTC_WDT, 0x0083)          # service (w1c status)
        serviced = qt.rw(MMIO_RTC_WDT)
    finally:
        p.kill(); p.wait()
    # after fire: status bit 0x80 set, count 2 (0x0200); IPR WDT = bit 3
    ok = (before & 0x0080) == 0 and (fired & 0x0280) == 0x0280 \
        and (ipr & 0x8) and (serviced & 0x0080) == 0
    print(f'  Watchdog: pre=0x{before:04x} fired=0x{fired:04x} '
          f'ipr=0x{ipr:x} serviced=0x{serviced:04x}  '
          f'[{"ok" if ok else "FAIL"}]')
    return ok


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    qemu, rom = argv[1], argv[2]
    with tempfile.TemporaryDirectory() as tmp:
        results = [
            test_pwm(qemu, rom, tmp),
            test_watchdog(qemu, rom, tmp),
        ]
    print('ALL PASS' if all(results) else 'SOME FAILED')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
