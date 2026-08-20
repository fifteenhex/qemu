import sys
from capstone import *
BASE=0x00000400   # the ROM runs shadowed in RAM at 0x400
d=open('rom_be.bin','rb').read()
md=Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN|CS_MODE_M68K_020)
start=int(sys.argv[1],0) if len(sys.argv)>1 else 0xb4
end=int(sys.argv[2],0) if len(sys.argv)>2 else 0x1c48
pc=start
out=[]
while pc<end:
    got=False
    for i in md.disasm(d[pc:end], BASE+pc, count=1):
        out.append("%08x  %-20s %s %s" % (i.address, d[pc:pc+i.size].hex(), i.mnemonic, i.op_str))
        pc+=i.size; got=True
    if not got:
        out.append("%08x  %-20s dc.w $%04x" % (BASE+pc, d[pc:pc+2].hex(), int.from_bytes(d[pc:pc+2],'big')))
        pc+=2
print("\n".join(out))
