#!/usr/bin/env python3
"""
e17probe.py - read-only RMON probe for the real ELTEC Eurocom E17 board.

Purpose: dump the hardware registers and board state we have modelled or
guessed in QEMU, so the real board's values can be compared against the
emulator.  Everything here is READ-ONLY: it issues only RMON display
commands ($, db, dl) plus a SCSI bus scan.  It never writes memory or
NVRAM, never boots, never uses sload/gm, and deliberately skips the
CD2401 console (0xfec64000) and its interrupt-acknowledge window
(0xfec66000) so it cannot disturb the serial link.

PREREQUISITE: the board's console must be on Serial Port 1 (i.e. video
not fitted, or setup Input/Output Port = Serial Port 1) so RMON's "***>"
prompt is on the serial line this script talks to.

Usage on the real board (Linux/macOS, no extra packages needed):
    python3 e17probe.py /dev/ttyUSB0            # default 9600 8N1
    python3 e17probe.py /dev/ttyUSB0 19200      # other baud
If the output is garbage, the baud is wrong - check setup Serial menu,
common values 9600 / 19200 / 38400 / 115200.

It writes a timestamped log (e17probe-<host>-<time>.txt); send that back.

(Internal: `python3 e17probe.py --socket /path/to.sock` drives the same
commands against a QEMU unix serial socket to produce the reference.)
"""
import sys, os, time, subprocess, socket, select, datetime

# (label, command, notes) - all read-only
COMMANDS = [
    ('version',        '$',              'RMON version / build'),
    ('csunit',         'dl fec70000 2c', 'chip-select + memory controller regs'),
    ('vic068a',        'dl fec01000 20', 'VIC068A regs (value = byte lane 3 of each long)'),
    ('asic_dramc',     'dl fec080f0 2',  'ASIC/DRAM timing regs'),
    ('cputype',        'db fec5e000 4',  'CPU-type latch (1=040, 4=060)'),
    ('misc_5c',        'db fec5c000 4',  'misc / "slave select"'),
    ('status_ack',     'db fec50000 4',  'status/ack (mild read side effect)'),
    ('i2c_ipin',       'db fec54000 4',  'I2C IPIN EEPROM port'),
    ('ramdac',         'db fec40000 8',  'RAMDAC; +2 should read 0x3a'),
    ('crtc',           'dl fec48000 8',  'CRTC direct regs'),
    ('macprom',        'db fec69d00 88', 'LANCE station-address PROM area'),
    ('nvram_hdr',      'db fec20000 20', 'NVRAM config header'),
    ('nvram_boardid',  'db fec20460 40', 'board ID block (+0x468): MAC/serial source'),
    ('nvram_cksum',    'db fec205e0 20', 'config checksum (last bytes at 5fd-5ff)'),
    ('nvram_bootstrap','db fec20700 20', 'OS-9 bootstrap param block (+0x700)'),
    ('rtc',            'db fec207f8 8',  'M48T02 clock: ctl,sec,min,hr,dow,date,mon,yr (BCD)'),
    ('scsi',           'scsi',           'SCSI bus scan (real targets/LUNs)'),
    # Full NVRAM dump is the richest single artifact but slow at low baud;
    # uncomment to include:
    # ('nvram_full',   'db fec20000 800','entire 2KB NVRAM/RTC'),
]

PROMPT = b'***>'


class SerialLink:
    def __init__(self, dev, baud):
        # configure the tty with stty, then do raw file I/O (no pyserial)
        subprocess.run(['stty', '-F', dev, str(baud), 'cs8', '-cstopb',
                        '-parenb', 'raw', '-echo', '-ixon', '-ixoff',
                        'clocal', '-crtscts'], check=True)
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    def send(self, data): os.write(self.fd, data)
    def recv(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        return os.read(self.fd, 4096) if r else b''
    def close(self): os.close(self.fd)


class SocketLink:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(path)
    def send(self, data): self.s.sendall(data)
    def recv(self, timeout):
        self.s.settimeout(timeout)
        try: return self.s.recv(4096)
        except socket.timeout: return b''
    def close(self): self.s.close()


def read_until_prompt(link, log, overall=25.0, echo=True):
    """Read until the RMON prompt reappears or we go quiet.

    Echoes the board's reply to the terminal as it arrives (echo=True) so
    you can watch the exchange live; the same bytes go to the log file.
    """
    buf = b''
    t0 = time.time()
    while time.time() - t0 < overall:
        chunk = link.recv(1.0)
        if chunk:
            buf += chunk
            if echo:
                # show exactly what came back, verbatim
                sys.stdout.write(chunk.decode('latin1'))
                sys.stdout.flush()
            if PROMPT in buf and time.time() - t0 > 0.3:
                break
        elif buf:
            break   # went quiet with data in hand
    text = buf.decode('latin1')
    log.write(text)
    return text


def send(link, log, data):
    """Send bytes to the board, echoing them to the terminal as '>>> ...'."""
    shown = data.decode('latin1').replace('\r', '')
    if shown:
        sys.stdout.write(f'\n>>> {shown}\n')
        sys.stdout.flush()
    log.write(f'>>> {shown}\n')
    link.send(data)


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == '--socket':
        link = SocketLink(sys.argv[2]); tag = 'qemu'
    else:
        dev = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
        baud = int(sys.argv[2]) if len(sys.argv) > 2 else 9600
        link = SerialLink(dev, baud); tag = os.uname().nodename
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    logpath = f'e17probe-{tag}-{stamp}.txt'
    log = open(logpath, 'w')
    log.write(f'# e17probe {stamp} target={tag}\n')

    # wake the prompt
    send(link, log, b'\r')
    banner = read_until_prompt(link, log, overall=8.0)
    if PROMPT not in banner.encode():
        print('\nWARNING: no "***>" prompt seen - wrong baud or console not '
              'on serial? Continuing anyway.', flush=True)

    for label, cmd, note in COMMANDS:
        log.write(f'\n===== {label}: {cmd}   ({note}) =====\n')
        sys.stdout.write(f'\n----- {label}  ({note}) -----')
        send(link, log, cmd.encode() + b'\r')
        # scsi can take a while (per-target selection timeouts)
        read_until_prompt(link, log, overall=90.0 if cmd == 'scsi' else 25.0)

    log.close(); link.close()
    print(f'\nDONE. Send back: {logpath}', flush=True)


if __name__ == '__main__':
    main()
