#!/usr/bin/env python3
"""
mboxprobe.py - probe the ELTEC E17 dual-CPU shared-memory "mailbox".

It loads a tiny DRAM-only payload onto the SECOND CPU and then talks to
it from RMON (running on CPU1) purely through shared DRAM, to work out
how the two 68040s communicate.  The payload (cpu2mbox.srec) only ever
touches DRAM - no I/O, no VMEbus - so it cannot disturb other boards.

Mailbox longwords in DRAM:
    0x00010100  command    CPU1 writes
    0x00010104  response   CPU2 writes  (= ~command, proof it saw it)
    0x00010108  heartbeat  CPU2 increments every loop (proof it runs)
    0x0001010c  quit       CPU1 writes nonzero -> CPU2 parks in STOP

Sequence driven over the RMON serial console:
    sload the payload (payload at 0x10000, SP/PC vector at 0/4)
    pulse 0xfec58000: mb 0 (assert slave reset), mb 0x20 (release)
      -> CPU2 fetches SP/PC from DRAM 0/4 and runs the payload
    read heartbeat (dl 10108) a few times          -> is CPU2 alive?
    write command (ml 10100 <v>), read response     -> shared-mem RTT
    write quit (ml 1010c 1)                          -> park CPU2

Transports (same as e17probe.py):
    python3 mboxprobe.py /dev/ttyUSB0 [baud]     # real board
    python3 mboxprobe.py --socket /path.sock     # QEMU

CAUTION for the real board: this resets and restarts the second CPU via
0xfec58000.  It is DRAM-only and re-parks CPU2 with the quit flag at the
end, but if it is interrupted, CPU2 is left spinning in local DRAM
(harmless to the bus, cleared by a board reset).
"""
import sys, os, time, subprocess, socket, select

SREC = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cpu2mbox.srec')
PROMPT = b'***>'


class SerialLink:
    def __init__(self, dev, baud):
        subprocess.run(['stty', '-F', dev, str(baud), 'cs8', '-cstopb',
                        '-parenb', 'raw', '-echo', '-ixoff', 'clocal',
                        '-crtscts'], check=True)
        self.fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    def send(self, d): os.write(self.fd, d)
    def recv(self, t):
        r, _, _ = select.select([self.fd], [], [], t)
        return os.read(self.fd, 4096) if r else b''


class SocketLink:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX); self.s.connect(path)
    def send(self, d): self.s.sendall(d)
    def recv(self, t):
        self.s.settimeout(t)
        try: return self.s.recv(4096)
        except socket.timeout: return b''


def wait_prompt(link, overall=20.0, echo=True):
    buf = b''; t0 = time.time(); quiet = 0.0
    while time.time() - t0 < overall:
        c = link.recv(0.5)
        if c:
            quiet = 0.0; buf += c
            if echo:
                sys.stdout.write(c.decode('latin1')); sys.stdout.flush()
            if buf.rstrip().endswith(PROMPT):
                break
        else:
            quiet += 0.5
            if buf and quiet >= 3.0:
                break
    return buf.decode('latin1')


def drain(link):
    while link.recv(0.3):
        pass


def cmd(link, s, wait=15.0):
    drain(link)
    sys.stdout.write(f'\n>>> {s}\n'); sys.stdout.flush()
    link.send(s.encode() + b'\r')
    return wait_prompt(link, wait)


def stream_srec(link):
    """Upload the S-records honoring sload's XON/XOFF."""
    data = open(SREC, 'rb').read().splitlines(keepends=True)
    paused = False
    for ln in data:
        while True:
            r = link.recv(0.05)
            for b in r:
                if b == 0x13: paused = True
                elif b == 0x11: paused = False
            if not paused:
                break
            time.sleep(0.005)
        link.send(ln)


def main():
    if sys.argv[1] == '--socket':
        link = SocketLink(sys.argv[2])
    else:
        dev = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
        baud = int(sys.argv[2]) if len(sys.argv) > 2 else 9600
        link = SerialLink(dev, baud)

    link.send(b'\r'); wait_prompt(link, 8.0)

    print('\n--- load CPU2 payload ---')
    cmd(link, 'sload 1 0', 5.0)
    stream_srec(link)
    wait_prompt(link, 10.0)              # "Transfer complete"

    print('\n--- release CPU2 (reset + run via 0xfec58000) ---')
    cmd(link, 'mb fec58000 0', 4.0); cmd(link, '.', 3.0)
    cmd(link, 'mb fec58000 20', 4.0); cmd(link, '.', 3.0)

    print('\n--- heartbeat (should advance if CPU2 is running) ---')
    for _ in range(4):
        cmd(link, 'dl 10108 1', 4.0); time.sleep(0.5)

    print('\n--- mailbox round trip: write cmd, read response(=~cmd) ---')
    for val in ('12345678', 'a5a5a5a5'):
        cmd(link, f'ml 10100 {val}', 4.0); cmd(link, '.', 3.0)
        time.sleep(0.3)
        cmd(link, 'dl 10100 4', 4.0)     # cmd, response, heartbeat, quit

    print('\n--- park CPU2 (quit flag) ---')
    cmd(link, 'ml 1010c 1', 4.0); cmd(link, '.', 3.0)
    time.sleep(0.3)
    cmd(link, 'dl 10108 1', 4.0)         # heartbeat should now be frozen
    time.sleep(0.5)
    cmd(link, 'dl 10108 1', 4.0)
    print('\nDONE')


if __name__ == '__main__':
    main()
