| SE/30 pseudo-slot $E minimal slot video driver (.Display_Video_Apple_SE30)
| Assembled with m68k-linux-gnu-as (MIT syntax), extracted flat with objcopy.
| Single fixed mode: 512x342 1-bit, framebuffer at slot VRAM base + $8040.
|
| Standard Mac DRVR resource: header (flags, offsets to Open/Prime/Ctl/
| Status/Close, driver name) followed by the routine bodies.
|
| Calling conventions that matter (learned the hard way, live-traced):
|  - Open/Close return via RTS with D0 = result.
|  - Prime/Control/Status must distinguish IMMEDIATE calls (noQueueBit set
|    in ioTrap, return via RTS) from QUEUED calls, which MUST complete by
|    jumping through JIODone (low-mem $8FC) with D0 = result and A1 = DCE;
|    a bare RTS on a queued call leaves ioResult stuck at 1 forever and the
|    System's synchronous-wait loop (ROM 0x40806c36) spins on the splash
|    screen for good.
|  - The framebuffer base is NOT hardcoded: the Slot Manager stores the
|    slot device base (minorBaseOS-derived, correct for the current 24/32
|    -bit addressing mode) in the AuxDCE dCtlDevBase (offset 42); we report
|    dCtlDevBase + $8040 for cscGetMode / cscGetBaseAddr so QuickDraw draws
|    exactly where macse30-fb scans, in either addressing mode.

    .set    JIODone,   0x8FC   | low-mem: Device Manager IODone jump vector
    .set    JVBLTask,  0xD28   | low-mem: Vertical Retrace Mgr slot-VBL vector
    .set    FB_OFFSET, 0x8040  | screen base offset inside slot space
    .set    VIDCTL_OFF, 0x80000 | card control regs offset inside slot space
    .set    NOQUEUEBIT, 1      | noQueueBit is bit 9 of the ioTrap WORD =
                               | bit 1 of its HIGH byte (big-endian, pb+6)

    .text
    .org 0
DRVR:
    .word   0x4c00              | drvrFlags: dNeedLock|dStatEnable|dCtlEnable
    .word   0                   | drvrDelay
    .word   0                   | drvrEMask
    .word   0                   | drvrMenu
    .word   Open - DRVR         | Open
    .word   Prime - DRVR        | Prime
    .word   Control - DRVR      | Control
    .word   Status - DRVR       | Status
    .word   Close - DRVR        | Close
| drvrName (Pascal string), word aligned
    .byte   NameEnd - NameStart
NameStart:
    .ascii  ".Display_Video_Apple_SE30"
NameEnd:
    .align  2

| ---------------------------------------------------------------------------
| Open: A0 = ioParam, A1 = DCE.  Record dCtlDevBase, install our slot $E
| interrupt handler (_SIntInstall) and arm the card's 60.15Hz VBL.  The
| driver blob runs from a heap pointer block, so PC-relative storage below
| is writable.  Returns via RTS with D0 = result (Open/Close never IODone).
| ---------------------------------------------------------------------------
Open:
    moveml  %d1-%d2/%a0-%a3,%sp@-
    lea     Installed(%pc),%a2
    tstb    %a2@                | already installed+armed?
    bne     OpenDone
    movel   %a1@(42),%d1        | AuxDCE dCtlDevBase
    lea     DevBase(%pc),%a2
    movel   %d1,%a2@
    | build the SlotIntQElement {qLink, sqType(6), sqPrio, sqAddr, sqParm}
    lea     IntQEl(%pc),%a2
    clrl    %a2@                | qLink
    movew   #6,%a2@(4)          | sqType = sIQType
    movew   #100,%a2@(6)        | sqPrio
    lea     IntHandler(%pc),%a3
    movel   %a3,%a2@(8)         | sqAddr = our handler
    lea     DevBase(%pc),%a3
    movel   %a3,%a2@(12)        | sqParm -> DevBase storage (A1 at int time)
    moveal  %a2,%a0             | A0 = SQElemPtr
    moveq   #0xE,%d0            | D0 = slot number
    .short  0xA075              | _SIntInstall
    tstw    %d0
    bne     OpenDone            | install failed: stay quiet (no VBL)
    | arm the card VBL (long write to vidctl +4)
    moveal  DevBase(%pc),%a2
    addal   #VIDCTL_OFF,%a2
    moveq   #1,%d1
    movel   %d1,%a2@(4)
    lea     Installed(%pc),%a2
    moveb   %d1,%a2@
OpenDone:
    moveml  %sp@+,%d1-%d2/%a0-%a3
    moveq   #0,%d0
    rts

| ---------------------------------------------------------------------------
| Slot $E interrupt handler (called by the ROM/OS slot interrupt dispatcher
| off VIA2 CA1/PA5).  A1 = sqParm = &DevBase.  Clear the card's VBL flag,
| run the slot VBL task queue via JVBLTask (D0.W = slot), return D0 != 0
| ("serviced").
| ---------------------------------------------------------------------------
IntHandler:
    moveml  %d1-%d2/%a1-%a3,%sp@-
    moveal  %a1@,%a0            | devbase
    addal   #VIDCTL_OFF,%a0
    clrl    %a0@                | any write to +0 clears the pending VBL
    | run slot VBL tasks if the Vertical Retrace Mgr vector looks sane
    movel   JVBLTask,%d1
    beq     IntNoVbl
    cmpil   #-1,%d1
    beq     IntNoVbl
    btst    #0,%d1
    bne     IntNoVbl
    moveal  %d1,%a0
    moveq   #0xE,%d0            | D0.W = slot number
    jsr     %a0@
IntNoVbl:
    moveml  %sp@+,%d1-%d2/%a1-%a3
    moveq   #1,%d0              | serviced
    rts

Close:
    moveq   #0,%d0
    rts

Prime:
    moveq   #0,%d0
    bra     Exit

| ---------------------------------------------------------------------------
| Control: A0 = cntrlParam, A1 = DCE.  csCode at A0@(26).
|   cscReset(0), cscKillIO(1), cscSetMode(2), cscSetEntries(3),
|   cscSetGamma(4), cscGrayPage(5), cscSetGray(6), cscSetInterrupt(7),
|   cscDirectSetEntries(8), cscSetDefaultMode(9) -> noErr.
|   Unknown -> controlErr (-17).
| ---------------------------------------------------------------------------
Control:
    movew   %a0@(26),%d1        | csCode
    cmpiw   #9,%d1
    bhi     CtlBad
    moveq   #0,%d0
    bra     Exit
CtlBad:
    moveq   #-17,%d0            | controlErr
    bra     Exit

| ---------------------------------------------------------------------------
| Status: A0 = cntrlParam, A1 = DCE.  csCode at A0@(26), csParam (Ptr) at
| A0@(28).  cscGetMode(2) fills VDPageInfo {csMode,csData,csPage,csBaseAddr};
| cscGetPageCnt(4) -> csPage=1; cscGetBaseAddr(5) -> csBaseAddr.
| ---------------------------------------------------------------------------
Status:
    movew   %a0@(26),%d1        | csCode
    moveml  %a1-%a2,%sp@-       | keep DCE (a1) intact for IODone
    moveal  %a0@(28),%a2        | csParam pointer -> VDPageInfo
    cmpiw   #2,%d1              | cscGetMode
    beq     StGetMode
    cmpiw   #4,%d1              | cscGetPageCnt
    beq     StGetPageCnt
    cmpiw   #5,%d1              | cscGetBaseAddr
    beq     StGetBase
    moveml  %sp@+,%a1-%a2
    moveq   #-18,%d0            | statusErr
    bra     Exit

StGetMode:
    | VDPageInfo: +0 csMode(w) +2 csData(l) +6 csPage(w) +8 csBaseAddr(l)
    movew   #0x80,%a2@(0)       | csMode = spID of our 1-bit mode ($80)
    movel   #0,%a2@(2)          | csData
    movew   #0,%a2@(6)          | csPage 0
    movel   %a1@(42),%d0        | AuxDCE dCtlDevBase (slot base, mode-correct)
    addil   #FB_OFFSET,%d0
    movel   %d0,%a2@(8)         | csBaseAddr
    moveml  %sp@+,%a1-%a2
    moveq   #0,%d0
    bra     Exit

StGetPageCnt:
    movew   #1,%a2@(6)          | csPage = 1 page
    moveml  %sp@+,%a1-%a2
    moveq   #0,%d0
    bra     Exit

StGetBase:
    movel   %a1@(42),%d0        | AuxDCE dCtlDevBase
    addil   #FB_OFFSET,%d0
    movel   %d0,%a2@(8)         | csBaseAddr
    moveml  %sp@+,%a1-%a2
    moveq   #0,%d0
    bra     Exit

| ---------------------------------------------------------------------------
| Common Prime/Control/Status exit: D0 = result, A0 = pb, A1 = DCE.
| Immediate call (noQueueBit set in ioTrap) -> plain RTS; queued call ->
| complete through JIODone (D0 = result, A1 = DCE).
| ---------------------------------------------------------------------------
Exit:
    btst    #NOQUEUEBIT,%a0@(6) | ioTrap high byte:bit9 -> byte at +6, bit 1
    beq     ExitQueued
    rts
ExitQueued:
    movel   JIODone,%sp@-       | jump through the IODone vector
    rts

| ---------------------------------------------------------------------------
| Writable driver-local storage (the driver runs from a heap block).
| ---------------------------------------------------------------------------
    .align  2
Installed:
    .byte   0                   | nonzero once the sInt handler is armed
    .byte   0
DevBase:
    .long   0                   | dCtlDevBase copy (sqParm points here)
IntQEl:
    .fill   16,1,0              | SlotIntQElement
