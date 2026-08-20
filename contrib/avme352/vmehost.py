#!/usr/bin/env python3
"""
Stand-in for the VMEbus master.  Talks to the QEMU gdbstub so that we can
read and write the AVME-352 dual ported RAM exactly the way a processor
board sitting on the same backplane would.
"""
import socket, time, sys

DPRAM = 0x10000000   # VMEbus slave window onto the dual ported RAM
CHAN_STRIDE = 0x280
CHAN0 = DPRAM + 0x080

CMD = DPRAM + 0x010          # per channel command byte
RXB = DPRAM + 0x000          # per channel single byte receive port
TXB = DPRAM + 0x008          # per channel single byte transmit port
IRQ = DPRAM + 0xFFC
EVT = DPRAM + 0xFFD
FATAL = DPRAM + 0xFFE
ECHAN = DPRAM + 0xFFF


class Gdb:
    def __init__(self, host='127.0.0.1', port=1234):
        self.s = socket.create_connection((host, port))
        self.s.settimeout(5)
        self.buf = b''
        self.stop()

    def _send(self, payload):
        csum = sum(payload.encode()) & 0xff
        self.s.sendall(b'$' + payload.encode() + b'#' + b'%02x' % csum)

    def _recv(self):
        while True:
            while b'#' not in self.buf or len(self.buf.split(b'#', 1)[1]) < 2:
                d = self.s.recv(4096)
                if not d:
                    raise EOFError
                self.buf += d
            if self.buf[0:1] == b'+':
                self.buf = self.buf[1:]
                continue
            assert self.buf[0:1] == b'$', self.buf[:20]
            body, rest = self.buf[1:].split(b'#', 1)
            self.buf = rest[2:]
            self.s.sendall(b'+')
            return body.decode()

    def cmd(self, payload):
        self._send(payload)
        return self._recv()

    def stop(self):
        self.s.sendall(b'\x03')
        try:
            self._recv()
        except socket.timeout:
            pass

    def cont(self):
        self._send('c')

    def read(self, addr, n):
        out = b''
        while n:
            k = min(n, 512)
            r = self.cmd('m%x,%x' % (addr, k))
            if r.startswith('E'):
                raise IOError(r)
            out += bytes.fromhex(r)
            addr += k
            n -= k
        return out

    def write(self, addr, data):
        while data:
            chunk, data = data[:512], data[512:]
            r = self.cmd('M%x,%x:%s' % (addr, len(chunk), chunk.hex()))
            if r != 'OK':
                raise IOError(r)
            addr += len(chunk)

    # run the guest for `secs` seconds of wall clock, then stop again
    def run(self, secs):
        self.cont()
        time.sleep(secs)
        self.stop()


class Card:
    def __init__(self, gdb):
        self.g = gdb

    def chan(self, n):
        return CHAN0 + n * CHAN_STRIDE

    def ident(self):
        return self.g.read(DPRAM + 0xF80, 32).split(b'\0')[0].decode()

    def command(self, ch, code, params=(), timeout=2.0):
        """Issue a command and wait for the card to write the status back."""
        base = self.chan(ch)
        blob = b''.join(int(p).to_bytes(2, 'big') for p in params)
        if blob:
            self.g.write(base + 0x18, blob)
        self.g.write(CMD + ch, bytes([code]))
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.g.run(0.05)
            st = self.g.read(CMD + ch, 1)[0]
            if st != code:
                return st
        raise TimeoutError('command 0x%02x on channel %d never completed' %
                           (code, ch))

    def status(self, ch):
        b = self.g.read(self.chan(ch), 0x54)
        return {
            'chan_id': b[0x00],
            'rx_err': b[0x01],
            'baud_idx': b[0x02], 'data_bits': b[0x03], 'stop_bits': b[0x04],
            'parity': b[0x05], 'flow': b[0x06], 'rts_cts': b[0x07],
            'evt_enable': b[0x08], 'modem_out': b[0x09], 'modem_in': b[0x0a],
            'misc': b[0x0b],
            'tx_free_init': int.from_bytes(b[0x0c:0x0e], 'big'),
            'rx_free_init': int.from_bytes(b[0x0e:0x10], 'big'),
            'tx_free': int.from_bytes(b[0x10:0x12], 'big'),
            'rx_count': int.from_bytes(b[0x12:0x14], 'big'),
            'evt_count': int.from_bytes(b[0x14:0x16], 'big'),
            'evt_lost': int.from_bytes(b[0x16:0x18], 'big'),
            'err_count': int.from_bytes(b[0x2e:0x32], 'big'),
        }

    def write_data(self, ch, data):
        base = self.chan(ch)
        self.g.write(base + 0x50, data)
        return self.command(ch, 0x18, [len(data)])

    def read_data(self, ch, n):
        base = self.chan(ch)
        st = self.command(ch, 0x17, [n])
        if st:
            return st, b''
        return st, self.g.read(base + 0x50, n)

    def mailbox(self):
        b = self.g.read(IRQ, 4)
        return {'irq': b[0], 'event': b[1], 'fatal': b[2], 'chan': b[3]}

    def ack_event(self):
        self.g.write(IRQ, b'\0')
