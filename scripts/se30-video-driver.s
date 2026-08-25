| SE/30 pseudo-slot $E minimal slot video driver (.Display_Video_SE30)
| Assembled with m68k-linux-gnu-as (MIT syntax), extracted flat with objcopy.
| Single fixed mode: 512x342 1-bit, framebuffer at VRAM base + $8040.
|
| Standard Mac DRVR resource: header (flags, offsets to Open/Prime/Ctl/
| Status/Close, driver name) followed by the routine bodies.  Control and
| Status are invoked immediately by the Display Manager / QuickDraw, so the
| routines return D0 = result via RTS.
|
| The framebuffer base is hardwired to the SE/30 pseudo-slot VRAM aperture
| (0x00F08040); the driver reports this for cscGetMode / cscGetBaseAddr so
| QuickDraw draws where macse30-fb scans.

    .text
    .org 0
DRVR:
    .word   0x4c00              | drvrFlags: dCtlEnable|dStatEnable|dNeedLock
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
    .ascii  ".Display_Video_SE30"
NameEnd:
    .align  2

| ---- framebuffer base constant ----
    .set    FB_BASE, 0xFE008040

| ---------------------------------------------------------------------------
| Open: A0 = ioParam, A1 = DCE.  Nothing to init; report success.
| ---------------------------------------------------------------------------
Open:
    moveq   #0,%d0
    rts

Prime:
    moveq   #0,%d0
    rts

Close:
    moveq   #0,%d0
    rts

| ---------------------------------------------------------------------------
| Control: A0 = cntrlParam, A1 = DCE.  csCode at A0@(26).
|   cscSetMode(2), cscSetEntries(3), cscSetGamma(4), cscGrayPage(5),
|   cscSetGray(6), cscSetInterrupt(7), cscSetDefaultMode(9) -> noErr.
|   Unknown -> controlErr (-17).
| ---------------------------------------------------------------------------
Control:
    movew   %a0@(26),%d1        | csCode
    cmpiw   #2,%d1
    beq     CtlOK
    cmpiw   #3,%d1
    beq     CtlOK
    cmpiw   #4,%d1
    beq     CtlOK
    cmpiw   #5,%d1
    beq     CtlOK
    cmpiw   #6,%d1
    beq     CtlOK
    cmpiw   #7,%d1
    beq     CtlOK
    cmpiw   #9,%d1
    beq     CtlOK
    cmpiw   #0,%d1              | cscReset
    beq     CtlOK
    moveq   #-17,%d0           | controlErr
    rts
CtlOK:
    moveq   #0,%d0
    rts

| ---------------------------------------------------------------------------
| Status: A0 = cntrlParam, A1 = DCE.  csCode at A0@(26), csParam (Ptr) at
| A0@(28).  cscGetMode(2) fills VDPageInfo {csMode,csData,csPage,csBaseAddr};
| cscGetPageCnt(4) -> csPage=1; cscGetBaseAddr(5) -> csBaseAddr=FB_BASE.
| ---------------------------------------------------------------------------
Status:
    movew   %a0@(26),%d1        | csCode
    moveal  %a0@(28),%a1        | csParam pointer -> VDPageInfo
    cmpiw   #2,%d1              | cscGetMode
    beq     StGetMode
    cmpiw   #4,%d1              | cscGetPageCnt
    beq     StGetPageCnt
    cmpiw   #5,%d1              | cscGetBaseAddr
    beq     StGetBase
    moveq   #-18,%d0           | statusErr
    rts

StGetMode:
    | VDPageInfo: +0 csMode(w) +2 csData(l) +6 csPage(w) +8 csBaseAddr(l)
    movew   #0x80,%a1@(0)       | csMode = spID of our 1-bit mode ($80)
    movel   #0,%a1@(2)          | csData
    movew   #0,%a1@(6)          | csPage 0
    movel   #FB_BASE,%a1@(8)    | csBaseAddr
    moveq   #0,%d0
    rts

StGetPageCnt:
    movew   #1,%a1@(6)          | csPage = 1 page
    moveq   #0,%d0
    rts

StGetBase:
    movel   #FB_BASE,%a1@(8)    | csBaseAddr
    moveq   #0,%d0
    rts
