#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Scripted playtest driver for the QEMU MegaDrive machine.

Talks QMP to a running qemu-system-m68k started with:
    -qmp unix:<socket>,server,nowait

Provides key injection (mapped like hw/input/md_io.c: arrows, a/s/d =
A/B/C, ret = Start), screendumps, and physical-memory peeks of the
guest (e.g. Sonic 1's Game_Mode byte at 0xFFF600).

Examples:
    # boot to title and start a game (Sonic 1 needs TWO Start edges if
    # a demo is running: the first only exits the demo)
    md-playtest.py --socket /tmp/md.qmp wait-title start shot /tmp/a.ppm

    # enter the Sonic 1 REV00 level select (title: U D L R, A+Start)
    # and pick an entry (downs: 3 = Labyrinth 1, 19 = Special Stage)
    md-playtest.py --socket /tmp/md.qmp wait-title levelselect 3

    # peek Sonic 1 game state
    md-playtest.py --socket /tmp/md.qmp peek 0xfff600 1
"""

import argparse
import json
import socket
import sys
import time


class QMP:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(path)
        self.f = self.sock.makefile('rw')
        self.f.readline()                       # greeting
        self.cmd('qmp_capabilities')

    def cmd(self, name, args=None):
        msg = {'execute': name}
        if args:
            msg['arguments'] = args
        self.f.write(json.dumps(msg) + '\n')
        self.f.flush()
        return json.loads(self.f.readline())

    def hmp(self, line):
        return self.cmd('human-monitor-command',
                        {'command-line': line}).get('return', '').strip()

    def key_event(self, qcode, down):
        self.cmd('input-send-event',
                 {'events': [{'type': 'key',
                              'data': {'down': down,
                                       'key': {'type': 'qcode',
                                               'data': qcode}}}]})

    def press(self, qcode, hold=0.15, gap=0.12):
        self.key_event(qcode, True)
        time.sleep(hold)
        self.key_event(qcode, False)
        time.sleep(gap)

    def peek_byte(self, addr):
        out = self.hmp('xp /1b 0x%x' % addr)
        return int(out.split()[-1], 16)

    def screendump(self, filename):
        self.cmd('screendump', {'filename': filename})


def wait_for_gamemode(q, value, timeout=30):
    """Sonic 1: Game_Mode at 0xFFF600 (0=Sega 4=title 8=demo 0xC=level)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if q.peek_byte(0xfff600) == value:
            return True
        time.sleep(0.25)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
             formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--socket', required=True, help='QMP unix socket path')
    ap.add_argument('actions', nargs='+',
                    help='wait-title | start | levelselect N | '
                         'press QCODE | hold QCODE SECS | shot FILE | '
                         'peek ADDR LEN | sleep SECS')
    args = ap.parse_args()

    q = QMP(args.socket)
    acts = args.actions
    i = 0
    while i < len(acts):
        a = acts[i]
        if a == 'wait-title':
            if not wait_for_gamemode(q, 0x04):
                sys.exit('timed out waiting for title screen')
            time.sleep(1.0)             # let it accept input
        elif a == 'start':
            q.press('ret')
        elif a == 'levelselect':
            i += 1
            downs = int(acts[i])
            for k in ('up', 'down', 'left', 'right'):
                q.press(k)
            time.sleep(0.5)
            q.key_event('a', True)
            time.sleep(0.3)
            q.press('ret')
            q.key_event('a', False)
            time.sleep(2)
            for _ in range(downs):
                q.press('down')
            q.press('ret')
        elif a == 'press':
            i += 1
            q.press(acts[i])
        elif a == 'hold':
            i += 1
            k = acts[i]
            i += 1
            q.key_event(k, True)
            time.sleep(float(acts[i]))
            q.key_event(k, False)
        elif a == 'shot':
            i += 1
            q.screendump(acts[i])
        elif a == 'peek':
            i += 1
            addr = int(acts[i], 0)
            i += 1
            print(q.hmp('xp /%db 0x%x' % (int(acts[i], 0), addr)))
        elif a == 'sleep':
            i += 1
            time.sleep(float(acts[i]))
        else:
            sys.exit('unknown action: %s' % a)
        i += 1


if __name__ == '__main__':
    main()
