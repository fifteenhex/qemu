#!/usr/bin/env python3
"""Drive a running Palm QEMU machine over its QMP socket.

Start the machine with a QMP unix socket, e.g.:

    qemu-system-m68k -M palmm500 -bios Palm-m500-4.1-en.rom \\
        -qmp unix:/tmp/palm.qmp,server,nowait -display none -serial null &

then send it a sequence of actions:

    palmctl.py /tmp/palm.qmp tap:16384,16384 sleep:2 dump:/tmp/s.ppm

Actions (applied in order):
    tap:X,Y      pen tap at absolute coords (0..32767 each axis)
    key:QCODE    press+release a key (QEMU qcode, e.g. f1, f7, up)
    down:X,Y     pen down and hold (no release)
    up           pen up
    sleep:S      wait S seconds
    dump:PATH    screendump to PATH (PPM)

Host key -> Palm mapping (see hw/input/palm_keypad.c):
    f1-f4  application buttons   f5 power   f6 contrast
    up/pgup, down/pgdn  scroll rocker
    f7-f10 silkscreen: Applications / Menu / Calculator / Find
"""
import json
import socket
import sys
import time


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(path)
        self.f = self.sock.makefile('rw')
        self.f.readline()                      # greeting
        self.cmd({'execute': 'qmp_capabilities'})

    def cmd(self, obj):
        self.f.write(json.dumps(obj) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                return r

    def send(self, *events):
        self.cmd({'execute': 'input-send-event',
                  'arguments': {'events': list(events)}})

    def abs(self, x, y):
        return [{'type': 'abs', 'data': {'axis': 'x', 'value': int(x)}},
                {'type': 'abs', 'data': {'axis': 'y', 'value': int(y)}}]

    def btn(self, down):
        return {'type': 'btn', 'data': {'button': 'left', 'down': down}}

    def tap(self, x, y):
        self.send(*self.abs(x, y), self.btn(True))
        time.sleep(0.3)
        self.send(self.btn(False))

    def key(self, qcode):
        for down in (True, False):
            self.send({'type': 'key',
                       'data': {'key': {'type': 'qcode', 'data': qcode},
                                'down': down}})
            time.sleep(0.05)

    def screendump(self, path):
        self.cmd({'execute': 'screendump', 'arguments': {'filename': path}})
        time.sleep(0.3)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    q = Qmp(argv[1])
    for a in argv[2:]:
        verb, _, arg = a.partition(':')
        if verb == 'tap':
            q.tap(*arg.split(','))
        elif verb == 'key':
            q.key(arg)
        elif verb == 'down':
            x, y = arg.split(',')
            q.send(*q.abs(x, y), q.btn(True))
        elif verb == 'up':
            q.send(q.btn(False))
        elif verb == 'sleep':
            time.sleep(float(arg))
        elif verb == 'dump':
            q.screendump(arg)
        else:
            print('unknown action:', a)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
