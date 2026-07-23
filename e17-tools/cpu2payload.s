| CPU2 mailbox payload for the ELTEC E17.  DRAM-only (no I/O, no VME),
| so it cannot disturb the bus.  Mailbox longwords (abs.l):
|   0x00010100  command   (CPU1 writes)
|   0x00010104  response  (CPU2 writes = ~command, proof it ran)
|   0x00010108  heartbeat (CPU2 increments each loop)
|   0x0001010c  quit      (CPU1 writes nonzero -> CPU2 parks in STOP)
    .text
    .globl start
start:
    moveq   #0,%d1
loop:
    addql   #1,%d1
    movel   %d1,0x10108
    movel   0x10100,%d0
    notl    %d0
    movel   %d0,0x10104
    tstl    0x1010c
    beq     loop
    stop    #0x2700
