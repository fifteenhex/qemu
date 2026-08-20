# AVME-352 reverse engineering notes

`AVME-352-programming.md` is the result: how a processor board on the
same VMEbus backplane drives an AVAL DATA AVME-352 six channel serial
card. Everything in it was derived from the `AVME-352 Ver 1.3` boot ROM
and checked by running that ROM under `qemu-system-m68k -M avme352`.

## Tools

`rom-disas.py` disassembles the ROM. The dumps in circulation have the
two bytes of every 16-bit word swapped, so de-swap first:

```sh
python3 -c "
d=open('avme352-ver1dot3.bin','rb').read(); s=bytearray(d)
s[0::2],s[1::2]=d[1::2],d[0::2]
open('rom_be.bin','wb').write(bytes(s))"
python3 rom-disas.py 0xb4 0x1c48 > rom.dis      # needs capstone
```

The base address in `rom-disas.py` is `0x400`, not `0x00C00000`: the ROM
copies its first 64 KiB down to local RAM at `0x400` and runs from there,
so every absolute address in the code is a RAM address.

`vmehost.py` is the other half — it stands in for the VMEbus master. It
talks to the QEMU gdbstub and reads and writes the dual ported RAM the
way a processor board would:

```sh
qemu-system-m68k -M avme352 -bios avme352-ver1dot3.bin -display none \
    -chardev socket,id=c0,path=/tmp/ch0.sock,server=on,wait=off \
    -serial chardev:c0 ... -gdb tcp::1234 &

python3 -c "
from vmehost import *
g = Gdb(); c = Card(g)
print(c.ident())
print(hex(c.command(0, 0x01, [8, 0, 0, 0, 0])))   # open ch0 at 9600 8N1
print(hex(c.write_data(0, b'hello\r\n')))
g.cont()"
```

QEMU has no VMEbus, so the slave window is parked at `0x10000000` in the
emulated address space.
