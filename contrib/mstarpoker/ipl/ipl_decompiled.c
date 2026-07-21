/*
 * Decompiled SSD202D IPL (first-stage loader).
 *
 * Reverse-engineered from the stock Miyoo Mini SPI-NOR image
 * (MiYoo283v1.1.bin, offset 0x0, "IPL_" image) with Ghidra 12.1.2 for
 * documentation and interoperability. Base 0xa0000000, ARM:LE:32:v7,
 * mixed ARM/Thumb. This is machine-generated output from proprietary
 * vendor firmware; it is NOT under the QEMU licence and is kept here
 * only as a reference for the model. See README.md for how it was
 * produced and validated against the emulated machine.
 */
/* IPL decompiled by Ghidra 12.1.2, base 0xa0000000, ARM:LE:32:v7 */

/*
 * UART0: a 16550 (register stride 8, i.e. reg-shift 3) the IPL uses for
 * its boot messages. Confirmed against the model: the IPL polls LSR bit 5
 * (0x1f221028) for the tx-holding register to empty, then writes the byte
 * to THR (0x1f221000).
 */
#define UART0_BASE     0x1f221000u
#define UART0_THR      (*(volatile unsigned char *)(UART0_BASE + 0x00)) /* tx holding reg */
#define UART0_LSR      (*(volatile unsigned char *)(UART0_BASE + 0x28)) /* line status reg */
#define UART_LSR_THRE  0x20u                                            /* THR empty */

/* thunk_FUN_a0000010 @ a0000000 */

void thunk_FUN_a0000010(void)

{
  uint uVar1;
  
  *DAT_a0000020 = DAT_a0000024;
  uVar1 = coproc_movefrom_Auxiliary_Control();
  coproc_moveto_Auxiliary_Control(uVar1 | 0x40);
  uVar1 = coproc_movefrom_NonSecure_Access_Control();
  coproc_moveto_NonSecure_Access_Control(uVar1 | 0xc00);
  coproc_moveto_Coprocessor_Access_Control(0xf00000);
  *DAT_a0000060 = DAT_a0000064;
  FUN_a0001d50(0x40000000);
  *DAT_a000007c = DAT_a0000080;
  do {
                    /* WARNING: Do nothing block with infinite loop */
  } while( true );
}



/* FUN_a0000010 @ a0000010 */

void FUN_a0000010(void)

{
  uint uVar1;
  
  *DAT_a0000020 = DAT_a0000024;
  uVar1 = coproc_movefrom_Auxiliary_Control();
  coproc_moveto_Auxiliary_Control(uVar1 | 0x40);
  uVar1 = coproc_movefrom_NonSecure_Access_Control();
  coproc_moveto_NonSecure_Access_Control(uVar1 | 0xc00);
  coproc_moveto_Coprocessor_Access_Control(0xf00000);
  *DAT_a0000060 = DAT_a0000064;
  FUN_a0001d50(0x40000000);
  *DAT_a000007c = DAT_a0000080;
  do {
                    /* WARNING: Do nothing block with infinite loop */
  } while( true );
}



/* FUN_a000014c @ a000014c */

/* WARNING: Control flow encountered bad instruction data */

void FUN_a000014c(undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4)

{
  bool bVar1;
  uint uVar2;
  uint uVar3;
  uint *puVar4;
  undefined4 extraout_r2;
  undefined4 extraout_r2_00;
  uint uVar5;
  uint uVar6;
  undefined4 extraout_r3;
  int iVar7;
  undefined4 extraout_r3_00;
  int iVar9;
  uint uVar10;
  uint uVar11;
  code *unaff_lr;
  undefined4 in_cr0;
  undefined4 in_cr7;
  undefined4 in_cr14;
  undefined8 uVar12;
  int iVar8;
  
  uVar6 = DAT_a000040c;
  uVar2 = uRama0000408;
  coproc_moveto_Translation_table_base_0(uRama0000408);
  iVar9 = 1;
  puVar4 = (uint *)(uRama0000408 & 0xfffffffc);
  uVar5 = DAT_a0000410;
  do {
    *puVar4 = uVar5;
    uVar5 = uVar5 + 0x100000;
    iVar9 = iVar9 + -1;
    puVar4 = puVar4 + 1;
  } while (iVar9 != 0);
  iVar9 = 8;
  uVar5 = uVar6 & 0x14000000 | DAT_a0000410;
  puVar4 = (uint *)(uVar2 & 0xfffffffc | 0x500);
  do {
    *puVar4 = uVar5;
    uVar5 = uVar5 + 0x100000;
    iVar9 = iVar9 + -1;
    puVar4 = puVar4 + 1;
  } while (iVar9 != 0);
  iVar9 = 4;
  uVar5 = uVar6 & 0x1f000000 | DAT_a0000414;
  puVar4 = (uint *)(uVar2 & 0xfffffffc | 0x7c0);
  do {
    *puVar4 = uVar5;
    uVar5 = uVar5 + 0x100000;
    iVar9 = iVar9 + -1;
    puVar4 = puVar4 + 1;
  } while (iVar9 != 0);
  iVar9 = 1;
  uVar5 = uVar6 & 0x1fc00000 | DAT_a0000410;
  puVar4 = (uint *)(uVar2 & 0xfffffffc | 0x7f0);
  do {
    *puVar4 = uVar5;
    uVar5 = uVar5 + 0x100000;
    iVar9 = iVar9 + -1;
    puVar4 = puVar4 + 1;
  } while (iVar9 != 0);
  iVar9 = 0x80;
  uVar5 = uVar6 & 0x20000000 | DAT_a0000410;
  puVar4 = (uint *)(uVar2 & 0xfffffffc | 0x800);
  do {
    *puVar4 = uVar5;
    uVar5 = uVar5 + 0x100000;
    iVar9 = iVar9 + -1;
    puVar4 = puVar4 + 1;
  } while (iVar9 != 0);
  iVar9 = 0x80;
  uVar5 = uVar6 & 0x20000000 | DAT_a0000414;
  puVar4 = (uint *)(uVar2 & 0xfffffffc | 0x1000);
  do {
    *puVar4 = uVar5;
    uVar5 = uVar5 + 0x100000;
    iVar9 = iVar9 + -1;
    puVar4 = puVar4 + 1;
  } while (iVar9 != 0);
  iVar9 = 0x80;
  uVar6 = uVar6 & 0xa0000000 | DAT_a0000410;
  puVar4 = (uint *)(uVar2 & 0xfffffffc | 0x2800);
  do {
    *puVar4 = uVar6;
    uVar6 = uVar6 + 0x100000;
    iVar9 = iVar9 + -1;
    puVar4 = puVar4 + 1;
  } while (iVar9 != 0);
  coproc_moveto_Domain_Access_Control(3);
  uVar2 = coproc_movefrom_Control();
  coproc_moveto_Control(uVar2 | 0x1005);
  (*unaff_lr)(param_1,param_2,param_3,param_4);
  uVar2 = coproc_movefrom_Control();
  coproc_moveto_Control(uVar2 & 0xfffffffa);
  uVar12 = FUN_a00002ec(uVar2 & 0xfffffffa);
  coprocessor_moveto(0xf,2,0,0,in_cr0,in_cr0);
  uVar2 = coprocessor_movefromRt(0xf,1,0,in_cr0,in_cr0);
  uVar6 = DAT_a000041c & uVar2 >> 3;
  iVar9 = (DAT_a0000418 & uVar2 >> 0xd) + 1;
  do {
    iVar9 = iVar9 + -1;
    iVar8 = uVar6 + 1;
    do {
      iVar7 = iVar8 + -1;
      coproc_moveto_Invalidate_Entire_Data_by_Index
                (iVar7 << LZCOUNT(uVar6) | iVar9 << (uVar2 & 7) + 4);
      bVar1 = 0 < iVar8;
      iVar8 = iVar7;
    } while (iVar7 != 0 && bVar1);
  } while (0 < iVar9);
  DataSynchronizationBarrier(0xe);
  InstructionSynchronizationBarrier(0xf);
  uVar12 = FUN_a0000300((int)uVar12,(int)((ulonglong)uVar12 >> 0x20),extraout_r2,extraout_r3);
  DataMemoryBarrier(0xf);
  uVar2 = coprocessor_movefromRt(0xf,1,1,in_cr0,in_cr0);
  if ((uVar2 & 0x7000000) != 0) {
    uVar6 = 0;
    do {
      if (1 < (uVar2 >> (uVar6 + (uVar6 >> 1) & 0xff) & 7)) {
        coprocessor_moveto(0xf,2,0,uVar6,in_cr0,in_cr0);
        InstructionSynchronizationBarrier(0xf);
        uVar3 = coprocessor_movefromRt(0xf,1,0,in_cr0,in_cr0);
        uVar10 = DAT_a000041c & uVar3 >> 3;
        uVar5 = uVar10;
        uVar11 = DAT_a0000418 & uVar3 >> 0xd;
        do {
          do {
            coprocessor_moveto(0xf,0,2,uVar6 | uVar5 << LZCOUNT(uVar10) | uVar11 << (uVar3 & 7) + 4,
                               in_cr7,in_cr14);
            bVar1 = 0 < (int)uVar5;
            uVar5 = uVar5 - 1;
          } while (bVar1);
          bVar1 = 0 < (int)uVar11;
          uVar5 = uVar10;
          uVar11 = uVar11 - 1;
        } while (bVar1);
      }
      uVar6 = uVar6 + 2;
    } while ((int)uVar6 < (int)((uVar2 & 0x7000000) >> 0x17));
  }
  coprocessor_moveto(0xf,2,0,0,in_cr0,in_cr0);
  DataSynchronizationBarrier(0xf);
  InstructionSynchronizationBarrier(0xf);
  FUN_a000036c((int)uVar12,(int)((ulonglong)uVar12 >> 0x20),extraout_r2_00,extraout_r3_00);
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}



/* FUN_a00002ec @ a00002ec */

/* WARNING: Control flow encountered bad instruction data */

void FUN_a00002ec(void)

{
  bool bVar1;
  uint uVar2;
  uint uVar3;
  undefined4 extraout_r2;
  int iVar4;
  undefined4 extraout_r2_00;
  undefined4 extraout_r3;
  uint uVar5;
  int iVar6;
  undefined4 extraout_r3_00;
  uint uVar8;
  uint uVar9;
  uint uVar10;
  code *unaff_lr;
  undefined4 in_cr0;
  undefined4 in_cr7;
  undefined4 in_cr14;
  undefined8 uVar11;
  int iVar7;
  
  uVar2 = coproc_movefrom_Control();
  coproc_moveto_Control(uVar2 & 0xfffffffa);
  uVar11 = (*unaff_lr)(uVar2 & 0xfffffffa);
  coprocessor_moveto(0xf,2,0,0,in_cr0,in_cr0);
  uVar2 = coprocessor_movefromRt(0xf,1,0,in_cr0,in_cr0);
  uVar5 = DAT_a000041c & uVar2 >> 3;
  iVar4 = (DAT_a0000418 & uVar2 >> 0xd) + 1;
  do {
    iVar4 = iVar4 + -1;
    iVar7 = uVar5 + 1;
    do {
      iVar6 = iVar7 + -1;
      coproc_moveto_Invalidate_Entire_Data_by_Index
                (iVar6 << LZCOUNT(uVar5) | iVar4 << (uVar2 & 7) + 4);
      bVar1 = 0 < iVar7;
      iVar7 = iVar6;
    } while (iVar6 != 0 && bVar1);
  } while (0 < iVar4);
  DataSynchronizationBarrier(0xe);
  InstructionSynchronizationBarrier(0xf);
  uVar11 = FUN_a0000300((int)uVar11,(int)((ulonglong)uVar11 >> 0x20),extraout_r2,extraout_r3);
  DataMemoryBarrier(0xf);
  uVar2 = coprocessor_movefromRt(0xf,1,1,in_cr0,in_cr0);
  if ((uVar2 & 0x7000000) != 0) {
    uVar5 = 0;
    do {
      if (1 < (uVar2 >> (uVar5 + (uVar5 >> 1) & 0xff) & 7)) {
        coprocessor_moveto(0xf,2,0,uVar5,in_cr0,in_cr0);
        InstructionSynchronizationBarrier(0xf);
        uVar3 = coprocessor_movefromRt(0xf,1,0,in_cr0,in_cr0);
        uVar8 = DAT_a000041c & uVar3 >> 3;
        uVar10 = uVar8;
        uVar9 = DAT_a0000418 & uVar3 >> 0xd;
        do {
          do {
            coprocessor_moveto(0xf,0,2,uVar5 | uVar10 << LZCOUNT(uVar8) | uVar9 << (uVar3 & 7) + 4,
                               in_cr7,in_cr14);
            bVar1 = 0 < (int)uVar10;
            uVar10 = uVar10 - 1;
          } while (bVar1);
          bVar1 = 0 < (int)uVar9;
          uVar10 = uVar8;
          uVar9 = uVar9 - 1;
        } while (bVar1);
      }
      uVar5 = uVar5 + 2;
    } while ((int)uVar5 < (int)((uVar2 & 0x7000000) >> 0x17));
  }
  coprocessor_moveto(0xf,2,0,0,in_cr0,in_cr0);
  DataSynchronizationBarrier(0xf);
  InstructionSynchronizationBarrier(0xf);
  FUN_a000036c((int)uVar11,(int)((ulonglong)uVar11 >> 0x20),extraout_r2_00,extraout_r3_00);
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}



/* FUN_a0000300 @ a0000300 */

/* WARNING: Control flow encountered bad instruction data */

void FUN_a0000300(undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4)

{
  bool bVar1;
  uint uVar2;
  uint uVar3;
  int iVar4;
  undefined4 extraout_r2;
  uint uVar5;
  int iVar6;
  undefined4 extraout_r3;
  uint uVar8;
  uint uVar9;
  uint uVar10;
  code *unaff_lr;
  undefined4 in_cr0;
  undefined4 in_cr7;
  undefined4 in_cr14;
  undefined8 uVar11;
  int iVar7;
  
  coprocessor_moveto(0xf,2,0,0,in_cr0,in_cr0);
  uVar2 = coprocessor_movefromRt(0xf,1,0,in_cr0,in_cr0);
  uVar5 = DAT_a000041c & uVar2 >> 3;
  iVar4 = (DAT_a0000418 & uVar2 >> 0xd) + 1;
  do {
    iVar4 = iVar4 + -1;
    iVar7 = uVar5 + 1;
    do {
      iVar6 = iVar7 + -1;
      coproc_moveto_Invalidate_Entire_Data_by_Index
                (iVar6 << LZCOUNT(uVar5) | iVar4 << (uVar2 & 7) + 4);
      bVar1 = 0 < iVar7;
      iVar7 = iVar6;
    } while (iVar6 != 0 && bVar1);
  } while (0 < iVar4);
  DataSynchronizationBarrier(0xe);
  InstructionSynchronizationBarrier(0xf);
  uVar11 = (*unaff_lr)(param_1,param_2,param_3,param_4);
  DataMemoryBarrier(0xf);
  uVar2 = coprocessor_movefromRt(0xf,1,1,in_cr0,in_cr0);
  if ((uVar2 & 0x7000000) != 0) {
    uVar5 = 0;
    do {
      if (1 < (uVar2 >> (uVar5 + (uVar5 >> 1) & 0xff) & 7)) {
        coprocessor_moveto(0xf,2,0,uVar5,in_cr0,in_cr0);
        InstructionSynchronizationBarrier(0xf);
        uVar3 = coprocessor_movefromRt(0xf,1,0,in_cr0,in_cr0);
        uVar8 = DAT_a000041c & uVar3 >> 3;
        uVar10 = uVar8;
        uVar9 = DAT_a0000418 & uVar3 >> 0xd;
        do {
          do {
            coprocessor_moveto(0xf,0,2,uVar5 | uVar10 << LZCOUNT(uVar8) | uVar9 << (uVar3 & 7) + 4,
                               in_cr7,in_cr14);
            bVar1 = 0 < (int)uVar10;
            uVar10 = uVar10 - 1;
          } while (bVar1);
          bVar1 = 0 < (int)uVar9;
          uVar10 = uVar8;
          uVar9 = uVar9 - 1;
        } while (bVar1);
      }
      uVar5 = uVar5 + 2;
    } while ((int)uVar5 < (int)((uVar2 & 0x7000000) >> 0x17));
  }
  coprocessor_moveto(0xf,2,0,0,in_cr0,in_cr0);
  DataSynchronizationBarrier(0xf);
  InstructionSynchronizationBarrier(0xf);
  FUN_a000036c((int)uVar11,(int)((ulonglong)uVar11 >> 0x20),extraout_r2,extraout_r3);
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}



/* FUN_a000036c @ a000036c */

/* WARNING: Control flow encountered bad instruction data */

void FUN_a000036c(undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4)

{
  bool bVar1;
  uint uVar2;
  uint uVar3;
  uint uVar4;
  uint uVar5;
  uint uVar6;
  uint uVar7;
  code *unaff_lr;
  undefined4 in_cr0;
  undefined4 in_cr7;
  undefined4 in_cr14;
  
  DataMemoryBarrier(0xf);
  uVar2 = coprocessor_movefromRt(0xf,1,1,in_cr0,in_cr0);
  if ((uVar2 & 0x7000000) != 0) {
    uVar7 = 0;
    do {
      if (1 < (uVar2 >> (uVar7 + (uVar7 >> 1) & 0xff) & 7)) {
        coprocessor_moveto(0xf,2,0,uVar7,in_cr0,in_cr0);
        InstructionSynchronizationBarrier(0xf);
        uVar3 = coprocessor_movefromRt(0xf,1,0,in_cr0,in_cr0);
        uVar4 = DAT_a000041c & uVar3 >> 3;
        uVar6 = uVar4;
        uVar5 = DAT_a0000418 & uVar3 >> 0xd;
        do {
          do {
            coprocessor_moveto(0xf,0,2,uVar7 | uVar6 << LZCOUNT(uVar4) | uVar5 << (uVar3 & 7) + 4,
                               in_cr7,in_cr14);
            bVar1 = 0 < (int)uVar6;
            uVar6 = uVar6 - 1;
          } while (bVar1);
          bVar1 = 0 < (int)uVar5;
          uVar6 = uVar4;
          uVar5 = uVar5 - 1;
        } while (bVar1);
      }
      uVar7 = uVar7 + 2;
    } while ((int)uVar7 < (int)((uVar2 & 0x7000000) >> 0x17));
  }
  coprocessor_moveto(0xf,2,0,0,in_cr0,in_cr0);
  DataSynchronizationBarrier(0xf);
  InstructionSynchronizationBarrier(0xf);
  (*unaff_lr)(param_1,param_2,param_3,param_4);
                    /* WARNING: Bad instruction - Truncating control flow here */
  halt_baddata();
}



/* FUN_a00008f0 @ a00008f0 */

/* WARNING: Restarted to delay deadcode elimination for space: stack */

uint * FUN_a00008f0(uint *param_1,byte param_2,uint param_3)

{
  uint *puVar1;
  uint *puVar2;
  uint uVar3;
  uint uVar4;
  uint uVar5;
  uint uVar6;
  bool bVar7;
  
  uVar6 = -(int)param_1 & 3U;
  if (param_3 < (-(int)param_1 & 3U)) {
    uVar6 = param_3;
  }
  uVar3 = (uint)param_2 << 0x18 | (uint)param_2 << 0x10;
  uVar4 = uVar3 | uVar3 >> 0x10;
  puVar1 = param_1;
  if ((bool)((byte)(uVar6 >> 1) & 1)) {
    *(byte *)param_1 = param_2;
    puVar1 = (uint *)((int)param_1 + 2);
    *(byte *)((int)param_1 + 1) = param_2;
  }
  puVar2 = puVar1;
  if ((int)(uVar6 << 0x1f) < 0) {
    puVar2 = (uint *)((int)puVar1 + 1);
    *(byte *)puVar1 = param_2;
  }
  uVar5 = param_3 - uVar6;
  if (uVar6 <= param_3 && uVar5 != 0) {
    uVar6 = -(int)puVar2 & 0x1c;
    puVar1 = puVar2;
    if (uVar6 != 0) {
      if (uVar5 < uVar6) {
        uVar6 = uVar5 & 0x1c;
      }
      uVar5 = uVar5 - uVar6;
      if (SUB41(uVar6 >> 4,0)) {
        *puVar2 = uVar4;
        puVar2[1] = uVar4;
        puVar2[2] = uVar4;
        puVar2[3] = uVar4;
        puVar2 = puVar2 + 4;
      }
      if ((int)(uVar6 << 0x1c) < 0) {
        *puVar2 = uVar4;
        puVar2[1] = uVar4;
        puVar2 = puVar2 + 2;
      }
      puVar1 = puVar2;
      if ((bool)((byte)((uVar6 << 0x1c) >> 0x1e) & 1)) {
        puVar1 = puVar2 + 1;
        *puVar2 = uVar4;
      }
    }
    uVar5 = uVar5 - 0x20;
    if (-1 < (int)uVar5) {
      do {
        bVar7 = 0x1f < uVar5;
        uVar5 = uVar5 - 0x20;
        *puVar1 = uVar4;
        puVar1[1] = uVar4;
        puVar1[2] = uVar4;
        puVar1[3] = uVar4;
        puVar1[4] = uVar4;
        puVar1[5] = uVar4;
        puVar1[6] = uVar4;
        puVar1[7] = uVar4;
        puVar1 = puVar1 + 8;
      } while (bVar7);
    }
    if ((bool)((byte)(uVar5 + 0x20 >> 4) & 1)) {
      *puVar1 = uVar4;
      puVar1[1] = uVar4;
      puVar1[2] = uVar4;
      puVar1[3] = uVar4;
      puVar1 = puVar1 + 4;
    }
    if ((int)(uVar5 << 0x1c) < 0) {
      *puVar1 = uVar4;
      puVar1[1] = uVar4;
      puVar1 = puVar1 + 2;
    }
    puVar2 = puVar1;
    if ((bool)((byte)((uVar5 << 0x1c) >> 0x1e) & 1)) {
      puVar2 = puVar1 + 1;
      *puVar1 = uVar4;
    }
    puVar1 = puVar2;
    if ((int)(uVar5 << 0x1e) < 0) {
      puVar1 = (uint *)((int)puVar2 + 2);
      *(short *)puVar2 = (short)(uVar3 >> 0x10);
    }
    if ((bool)((byte)((uVar5 << 0x1e) >> 0x1e) & 1)) {
      *(byte *)puVar1 = param_2;
    }
    return param_1;
  }
  return param_1;
}



/* FUN_a00009dc @ a00009dc */

undefined2 FUN_a00009dc(ushort param_1,byte param_2)

{
  undefined2 local_c;
  undefined1 local_9;
  
  local_c = 0;
  for (local_9 = 0; local_9 < param_2; local_9 = local_9 + 1) {
    local_c = *(undefined2 *)(((uint)param_1 * 2 + 0x112200) * 2 + 0x1f000000);
  }
  return local_c;
}



/* FUN_a0000a28 @ a0000a28 */

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void FUN_a0000a28(void)

{
  _DAT_1f2244a0 = _DAT_1f2244a0 | 1;
  FUN_a00009dc(0x28,1);
  _DAT_1f2244a0 = _DAT_1f2244a0 & 0xfffe;
  FUN_a00009dc(0x28,1);
  return;
}



/* FUN_a0000a78 @ a0000a78 */

void FUN_a0000a78(int param_1)

{
  ushort *puVar1;
  ushort uVar2;
  
  puVar1 = DAT_a0000ac0;
  if (param_1 == 1) {
    uVar2 = *DAT_a0000ac0 | 2;
  }
  else {
    uVar2 = *DAT_a0000ac0 & 0xfffd;
  }
  *DAT_a0000ac0 = uVar2;
  FUN_a00009dc(0x21,1);
  *puVar1 = *puVar1 | 4;
  FUN_a00009dc(0x21,1);
  *puVar1 = *puVar1 | 8;
  FUN_a00009dc(0x21,1);
  return;
}



/* FUN_a0000ac4 @ a0000ac4 */

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void FUN_a0000ac4(int param_1)

{
  undefined4 local_c;
  
  _DAT_1f224488 = 0x80;
  _DAT_1f224480 = _DAT_1f224480 | 1;
  for (local_c = 0x7f; -1 < local_c; local_c = local_c + -2) {
    _DAT_1f22448c = *(ushort *)(local_c * 2 + param_1) >> 8 | *(short *)(local_c * 2 + param_1) << 8
    ;
    FUN_a00009dc(0x23,1);
    _DAT_1f224490 =
         *(ushort *)((local_c + 0x7fffffff) * 2 + param_1) >> 8 |
         *(short *)((local_c + 0x7fffffff) * 2 + param_1) << 8;
    FUN_a00009dc(0x24,1);
  }
  _DAT_1f224480 = _DAT_1f224480 & 0xfffe;
  return;
}



/* FUN_a0000bac @ a0000bac */

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void FUN_a0000bac(void)

{
  _DAT_1f224488 = 0;
  FUN_a00009dc(0x22,1);
  _DAT_1f224480 = _DAT_1f224480 | 1;
  FUN_a00009dc(0x20,1);
  _DAT_1f22448c = 1;
  FUN_a00009dc(0x23,1);
  _DAT_1f224490 = 1;
  FUN_a00009dc(0x24,1);
  _DAT_1f224480 = _DAT_1f224480 & 0xfffe;
  FUN_a00009dc(0x20,1);
  return;
}



/* FUN_a0000c36 @ a0000c36 */

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void FUN_a0000c36(int param_1)

{
  undefined4 local_c;
  
  _DAT_1f224488 = 0x40;
  _DAT_1f224480 = _DAT_1f224480 | 1;
  for (local_c = 0; local_c < 0x40; local_c = local_c + 1) {
    _DAT_1f22448c =
         (ushort)(byte)((uint)*(undefined4 *)(local_c * -4 + 0xfc + param_1) >> 0x18) |
         (ushort)((uint)*(undefined4 *)(local_c * -4 + 0xfc + param_1) >> 8) & 0xff00;
    FUN_a00009dc(0x23,1);
    _DAT_1f224490 =
         (ushort)((*(uint *)(local_c * -4 + 0xfc + param_1) & 0xff) << 8) |
         (ushort)((uint)*(undefined4 *)(local_c * -4 + 0xfc + param_1) >> 8) & 0xff;
    FUN_a00009dc(0x24,1);
  }
  _DAT_1f224480 = _DAT_1f224480 & 0xfffe;
  return;
}



/* FUN_a0000d1c @ a0000d1c */

void FUN_a0000d1c(short param_1)

{
  *DAT_a0000d30 = *DAT_a0000d30 | param_1 << 8;
  FUN_a00009dc(0x28,1);
  return;
}



/* FUN_a0000d34 @ a0000d34 */

void FUN_a0000d34(int param_1,int param_2,undefined4 param_3,undefined4 param_4)

{
  ushort *puVar1;
  undefined4 extraout_r2;
  ushort uVar2;
  
  puVar1 = DAT_a0000d80;
  if (param_1 == 1) {
    uVar2 = *DAT_a0000d80 | 2;
  }
  else {
    uVar2 = *DAT_a0000d80 & 0xfffd;
  }
  *DAT_a0000d80 = uVar2;
  FUN_a00009dc(0x28,1);
  uVar2 = *puVar1;
  if (param_2 == 1) {
    uVar2 = uVar2 | 4;
  }
  else {
    uVar2 = uVar2 & 0xfffb;
  }
  *puVar1 = uVar2;
  FUN_a00009dc(0x28,1,extraout_r2,param_4);
  return;
}



/* FUN_a0000d84 @ a0000d84 */

void FUN_a0000d84(void)

{
  *DAT_a0000d90 = *DAT_a0000d90 | 1;
  return;
}



/* FUN_a0000d94 @ a0000d94 */

undefined2 FUN_a0000d94(void)

{
  return *DAT_a0000d9c;
}



/* FUN_a0000da0 @ a0000da0 */

void FUN_a0000da0(void)

{
  *DAT_a0000db4 = *DAT_a0000db4 | 1;
  FUN_a00009dc(0x20,1);
  return;
}



/* FUN_a0000db8 @ a0000db8 */

void FUN_a0000db8(void)

{
  *DAT_a0000dd0 = *DAT_a0000dd0 & 0xfffe;
  FUN_a00009dc(0x20,1);
  return;
}



/* FUN_a0000dd4 @ a0000dd4 */

void FUN_a0000dd4(short param_1)

{
  *DAT_a0000de4 = param_1 + 0xc0;
  FUN_a00009dc(0x22,1);
  return;
}



/* FUN_a0000de8 @ a0000de8 */

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

undefined4 FUN_a0000de8(void)

{
  undefined2 uVar1;
  
  FUN_a00009dc(0x25,2);
  uVar1 = _DAT_1f224494;
  FUN_a00009dc(0x26,2);
  return CONCAT22(_DAT_1f224498,uVar1);
}



/* FUN_a0000e2c @ a0000e2c */

void FUN_a0000e2c(void)

{
  ushort *puVar1;
  
  puVar1 = DAT_a0000e4c;
  *DAT_a0000e4c = *DAT_a0000e4c | 0x80;
  *puVar1 = *puVar1 & 0xff7f;
  *DAT_a0000e50 = 0;
  return;
}



/* FUN_a0000e54 @ a0000e54 */

void FUN_a0000e54(uint param_1)

{
  *DAT_a0000e84 = (short)(param_1 & 0xfffffff);
  FUN_a00009dc(10,1);
  *DAT_a0000e88 = (short)((param_1 & 0xfffffff) >> 0x10);
  FUN_a00009dc(0xb,1);
  *DAT_a0000e8c = *DAT_a0000e8c | 0x800;
  return;
}



/* FUN_a0000e90 @ a0000e90 */

void FUN_a0000e90(undefined4 param_1)

{
  *DAT_a0000eb4 = (short)param_1;
  FUN_a00009dc(0xc,1);
  *DAT_a0000eb8 = (short)((uint)param_1 >> 0x10);
  FUN_a00009dc(0xd,1);
  return;
}



/* FUN_a0000ebc @ a0000ebc */

void FUN_a0000ebc(int param_1)

{
  ushort uVar1;
  
  if (param_1 == 1) {
    uVar1 = *DAT_a0000ee0 | 0x200;
  }
  else {
    uVar1 = *DAT_a0000ee0 & 0xfdff;
  }
  *DAT_a0000ee0 = uVar1;
  FUN_a00009dc(8,1);
  return;
}



/* FUN_a0000ee4 @ a0000ee4 */

undefined2 FUN_a0000ee4(void)

{
  return *DAT_a0000eec;
}



/* FUN_a0000ef0 @ a0000ef0 */

void FUN_a0000ef0(void)

{
  ushort *puVar1;
  
  puVar1 = DAT_a0000f08;
  *DAT_a0000f08 = *DAT_a0000f08 & 0xffbf;
  *puVar1 = *puVar1 | 0x40;
  return;
}



/* FUN_a0000f0c @ a0000f0c */

void FUN_a0000f0c(int param_1)

{
  ushort uVar1;
  
  if (param_1 == 0) {
    uVar1 = *DAT_a0000f2c & 0xfffe;
  }
  else {
    uVar1 = *DAT_a0000f2c | 1;
  }
  *DAT_a0000f2c = uVar1;
  FUN_a00009dc(8,1);
  return;
}



/* FUN_a0000f30 @ a0000f30 */

void FUN_a0000f30(int param_1)

{
  undefined4 local_c;
  
  for (local_c = 0; local_c < 0x10; local_c = local_c + 1) {
    FUN_a00009dc((local_c & 0xffff) + 0x10 & 0xffff,2);
    *(undefined2 *)(param_1 + local_c * 2) = *(undefined2 *)((local_c + 0x89110) * 4 + 0x1f000000);
  }
  return;
}



/* FUN_a0000f7e @ a0000f7e */

void FUN_a0000f7e(undefined4 param_1,undefined4 param_2,int param_3,undefined4 param_4)

{
  uint uVar1;
  
  FUN_a0000e2c();
  FUN_a0000e54(param_1);
  FUN_a0000e90(param_2);
  if (param_3 == 0) {
    FUN_a0000ebc(0);
  }
  else {
    if (param_3 != 1) {
      return;
    }
    FUN_a0000ebc(1);
  }
  FUN_a0000f0c(1);
  do {
    uVar1 = FUN_a0000ee4();
  } while ((uVar1 & 1) != 1);
  FUN_a0000f30(param_4);
  FUN_a0000f0c(0);
  FUN_a0000ef0();
  FUN_a0000e2c();
  return;
}



/* FUN_a0000fe8 @ a0000fe8 */

void FUN_a0000fe8(undefined4 *param_1)

{
  uint uVar1;
  undefined4 uVar2;
  int iVar3;
  int local_18;
  int local_14;
  
  FUN_a0000a28();
  FUN_a0000d1c((param_1[5] & 0xffff) - 1 & 0x3f);
  FUN_a0000d34(*(undefined1 *)(param_1 + 4),*(undefined1 *)((int)param_1 + 0x11));
  FUN_a0000a78(0);
  FUN_a0000a78(1);
  FUN_a0000ac4(*param_1);
  if (*(char *)(param_1 + 4) == '\0') {
    if (param_1[1] != 0) {
      FUN_a0000c36(param_1[1]);
    }
    FUN_a0000bac();
  }
  FUN_a0000d84();
  do {
    uVar1 = FUN_a0000d94();
  } while ((uVar1 & 2) != 2);
  if ((*(char *)(param_1 + 4) == '\0') && (param_1[5] != 0x800)) {
    local_14 = 0x20;
  }
  else {
    local_14 = 0x40;
  }
  FUN_a0000a78(0);
  local_18 = 0;
  while( true ) {
    if (local_14 <= local_18) break;
    FUN_a0000dd4(local_18);
    FUN_a0000da0();
    iVar3 = param_1[3];
    uVar2 = FUN_a0000de8();
    *(undefined4 *)(iVar3 + local_18 * 4) = uVar2;
    local_18 = local_18 + 1;
  }
  FUN_a0000db8();
  FUN_a0000a28();
  return;
}



/* FUN_a00010c2 @ a00010c2 */

undefined4 FUN_a00010c2(undefined4 param_1,undefined4 param_2,undefined4 param_3,int *param_4)

{
  int *piVar1;
  char cVar2;
  undefined2 uVar3;
  int *piVar4;
  undefined4 local_148;
  int *piStack_144;
  undefined4 uStack_140;
  int *local_13c;
  undefined4 local_138;
  int *piStack_134;
  int aiStack_130 [8];
  int aiStack_110 [64];
  
  piStack_144 = *(int **)((undefined1  [16])0x0 + (undefined1  [16])0x4);
  uStack_140 = *(undefined4 *)((undefined1  [16])0x0 + (undefined1  [16])0x8);
  local_13c = *(int **)((undefined1  [16])0x0 + (undefined1  [16])0xc);
  local_148 = 0;
  local_138 = 0;
  piStack_134 = piStack_144;
  FUN_a0000f7e(param_1,param_2,1,aiStack_130);
  if ((param_4 == (int *)0x0) || (*param_4 == 0)) {
    uVar3 = 0x101;
  }
  else {
    piStack_134 = (int *)0x800;
    uVar3 = 0x100;
    piStack_144 = param_4;
  }
  local_138 = CONCAT22(local_138._2_2_,uVar3);
  local_148 = param_3;
  local_13c = aiStack_110;
  FUN_a0000fe8(&local_148);
  cVar2 = '\b';
  piVar1 = aiStack_130;
  piVar4 = aiStack_110;
  do {
    if (*piVar1 != *piVar4) {
      return 0;
    }
    cVar2 = cVar2 + -1;
    piVar1 = piVar1 + 1;
    piVar4 = piVar4 + 1;
  } while (cVar2 != '\0');
  return 1;
}



/* FUN_a0001128 @ a0001128 */

void FUN_a0001128(uint param_1,ushort *param_2,uint param_3,int param_4)

{
  ushort uVar1;
  int iVar2;
  uint uVar3;
  uint uVar4;
  
  uVar4 = 1 << (param_1 & 0xff);
  uVar3 = 0xf << (param_3 & 0xff);
  iVar2 = param_4 + (((int)(*param_2 & uVar3) >> (param_3 & 0xff) |
                     ((int)(uVar4 & *DAT_a0001198) >> (param_1 & 0xff)) << 4) & 0xffffU);
  if ((param_4 < 0) && (iVar2 < 0)) {
    uVar1 = 0;
  }
  else {
    uVar1 = (ushort)iVar2;
  }
  *DAT_a0001198 =
       (ushort)(((uVar1 & 0x1f) >> 4) << (param_1 & 0xff)) | *DAT_a0001198 & ~(ushort)uVar4;
  *param_2 = *param_2 & ~(ushort)uVar3 | (ushort)((uVar1 & 0xf) << (param_3 & 0xff));
  return;
}



/* FUN_a000119c @ a000119c */

bool FUN_a000119c(void)

{
  uint uVar1;
  
  *DAT_a00011c0 = *DAT_a00011c0 & 0xfeff;
  uVar1 = (*DAT_a00011c4 & 0x1f) >> 3;
  if (uVar1 != 0) {
    return uVar1 == 3;
  }
  return true;
}



/* FUN_a00011c8 @ a00011c8 */

undefined4 FUN_a00011c8(uint param_1)

{
  undefined2 *puVar1;
  
  if (param_1 < 8) {
    *DAT_a0001238 = *DAT_a0001238 & 0xfeff;
    puVar1 = DAT_a000123c;
    if (param_1 == 0) goto LAB_a00011dc;
  }
  else {
    *DAT_a0001238 = *DAT_a0001238 | 0x100;
    puVar1 = DAT_a000123c;
    if (param_1 == 8) goto LAB_a00011dc;
  }
  param_1 = param_1 & 0xfffffff7;
  puVar1 = DAT_a0001240;
  if ((((param_1 != 1) && (puVar1 = DAT_a0001244, param_1 != 2)) &&
      (puVar1 = DAT_a0001248, param_1 != 3)) &&
     (((puVar1 = DAT_a000124c, param_1 != 4 && (puVar1 = DAT_a0001250, param_1 != 5)) &&
      ((puVar1 = DAT_a0001254, param_1 != 6 && (puVar1 = DAT_a0001258, param_1 != 7)))))) {
    return 0;
  }
LAB_a00011dc:
  return CONCAT22(puVar1[2],*puVar1);
}



/* FUN_a000128c @ a000128c */

void FUN_a000128c(undefined2 param_1,int param_2,uint param_3,int param_4)

{
  undefined2 *puVar1;
  uint uVar2;
  int iVar3;
  uint uVar4;
  ushort *puVar5;
  undefined4 in_cr6;
  undefined4 in_cr7;
  
  iVar3 = *DAT_a0001334;
  if ((iVar3 != 0) && (*(code **)(iVar3 + 0xc) != (code *)0x0)) {
    (**(code **)(iVar3 + 0xc))(*(undefined1 *)(iVar3 + 4));
  }
  puVar1 = DAT_a0001338;
  uVar2 = (param_3 + param_2 & 0xffffffc0) - (param_3 & 0xffffffc0) >> 6;
  uVar4 = param_3;
  if ((param_3 + param_2 & 0x3f) != 0) {
    uVar2 = uVar2 + 1;
  }
  while (uVar2 != 0) {
    uVar2 = uVar2 - 1;
    coprocessor_moveto(0xf,0,1,uVar4,in_cr7,in_cr6);
    uVar4 = uVar4 + 0x40;
  }
  *DAT_a0001338 = 0;
  puVar1[4] = 0x4035;
  if (param_4 == 0) {
    *DAT_a000133c = 0x2000;
  }
  else {
    *DAT_a000133c = 0;
  }
  puVar1 = DAT_a0001340;
  *DAT_a0001344 = param_1;
  *puVar1 = 0;
  puVar1[2] = (short)param_3;
  puVar1[4] = (ushort)((param_3 << 4) >> 0x14);
  puVar1[6] = (short)param_2;
  puVar1[8] = 0;
  puVar5 = DAT_a0001338 + 2;
  *DAT_a0001338 = 1;
  do {
  } while (-1 < (int)((uint)*puVar5 << 0x1c));
  *(undefined1 *)puVar5 = 8;
  puVar1 = DAT_a0001348;
  *DAT_a0001348 = 0x3800;
  puVar1[-0x1a] = 0x17;
  puVar1[-0x16] = 0;
  return;
}



/* FUN_a000134c @ a000134c */

void FUN_a000134c(int param_1,uint param_2)

{
  uint uVar1;
  uint uVar2;
  ushort *puVar3;
  int iVar4;
  
  uVar1 = 0;
  while (uVar2 = (uVar1 & 0x7f) * 2, uVar2 < param_2) {
    puVar3 = (ushort *)(DAT_a0001378 + uVar1 * 4);
    uVar1 = uVar1 + 1 & 0xff;
    *(char *)(param_1 + uVar2) = (char)*puVar3;
    iVar4 = uVar2 + 1;
    if (iVar4 < (int)param_2) {
      uVar2 = uVar2 + param_1;
      puVar3 = (ushort *)(uint)(*puVar3 >> 8);
    }
    if (iVar4 < (int)param_2) {
      *(char *)(uVar2 + 1) = (char)puVar3;
    }
  }
  return;
}



/* FUN_a000137c @ a000137c */

void FUN_a000137c(void)

{
  short sVar1;
  
  sVar1 = 100;
  do {
    if ((*DAT_a0001394 & 1) != 0) {
      return;
    }
    sVar1 = sVar1 + -1;
  } while (sVar1 != 0);
  return;
}



/* FUN_a0001398 @ a0001398 */

undefined4 FUN_a0001398(undefined4 param_1)

{
  undefined2 *puVar1;
  undefined2 *puVar2;
  ushort *puVar3;
  undefined4 uVar4;
  undefined8 uVar5;
  
  puVar2 = DAT_a00013e8;
  puVar1 = DAT_a00013e4;
  *DAT_a00013e8 = 1;
  *puVar1 = 0;
  *puVar1 = 0x8007;
  puVar1[0x12] = 0;
  puVar1[0x14] = 0;
  puVar1[-0x18] = 0x1306;
  *DAT_a00013ec = (ushort)param_1 & 0xff00 | (ushort)(byte)((uint)param_1 >> 0x10);
  puVar3 = DAT_a00013f0;
  *DAT_a00013f0 = (ushort)param_1 & 0xff;
  puVar3[0x10] = 0x41;
  puVar3[0x12] = 0;
  *DAT_a00013f4 = 1;
  uVar5 = FUN_a000137c();
  uVar4 = 0;
  if ((int)uVar5 != 0) {
    *puVar2 = (short)((ulonglong)uVar5 >> 0x20);
    uVar4 = (int)((ulonglong)uVar5 >> 0x20);
  }
  return uVar4;
}



/* FUN_a00013f8 @ a00013f8 */

undefined4 FUN_a00013f8(undefined4 param_1)

{
  undefined2 *puVar1;
  undefined4 uVar2;
  undefined6 uVar3;
  
  puVar1 = DAT_a0001438;
  *DAT_a0001438 = 7;
  puVar1[0x12] = 0;
  uVar2 = 2;
  puVar1[0x14] = 0;
  puVar1[-0x18] = 0xc00f;
  puVar1[-4] = 2;
  puVar1[-2] = 1;
  puVar1[2] = 1;
  uVar3 = FUN_a000137c();
  if ((int)uVar3 != 0) {
    *DAT_a000143c = (short)((uint6)uVar3 >> 0x20);
    FUN_a000134c(param_1);
    uVar2 = 0;
  }
  return uVar2;
}



/* FUN_a0001440 @ a0001440 */

undefined4 FUN_a0001440(undefined4 param_1,uint3 param_2)

{
  undefined2 *puVar1;
  char cVar2;
  undefined4 uVar3;
  short sVar4;
  undefined6 uVar5;
  undefined4 uStack_c;
  
  puVar1 = DAT_a000149c;
  uStack_c = (uint)param_2;
  *DAT_a000149c = 7;
  puVar1[0x12] = 0;
  puVar1[0x14] = 0;
  puVar1[-0x18] = 0xff;
  puVar1[-4] = 1;
  puVar1[-2] = 0;
  *DAT_a00014a0 = 1;
  uVar5 = FUN_a000137c();
  if ((int)uVar5 == 0) {
LAB_a0001498:
    uVar3 = 0;
  }
  else {
    sVar4 = 0x2711;
    *DAT_a00014a4 = (short)((uint6)uVar5 >> 0x20);
    do {
      sVar4 = sVar4 + -1;
      if (sVar4 == 0) goto LAB_a0001498;
      cVar2 = FUN_a00013f8((int)&uStack_c + 3);
    } while (cVar2 != '\0' || (uStack_c & 0x1000000) != 0);
    uVar3 = 1;
  }
  return uVar3;
}



/* FUN_a00014a8 @ a00014a8 */

undefined4 FUN_a00014a8(byte *param_1,short param_2)

{
  undefined2 *puVar1;
  undefined4 uVar2;
  undefined6 uVar3;
  
  puVar1 = DAT_a00014e8;
  *DAT_a00014e8 = 7;
  puVar1[0x12] = 0;
  puVar1[0x14] = 0;
  puVar1[-0x18] = param_2 << 8 | 0x1f;
  puVar1[-0x16] = (ushort)*param_1;
  puVar1[-4] = 3;
  puVar1[-2] = 0;
  puVar1[2] = 1;
  uVar3 = FUN_a000137c();
  if ((int)uVar3 == 0) {
    uVar2 = 2;
  }
  else {
    uVar2 = 0;
    *DAT_a00014ec = (short)((uint6)uVar3 >> 0x20);
  }
  return uVar2;
}



/* FUN_a00014f0 @ a00014f0 */

undefined4 FUN_a00014f0(undefined4 param_1,short param_2)

{
  undefined2 *puVar1;
  undefined4 uVar2;
  undefined8 uVar3;
  
  puVar1 = DAT_a0001534;
  *DAT_a0001534 = 7;
  puVar1[0x12] = 0;
  puVar1[0x14] = 0;
  puVar1[-0x18] = param_2 << 8 | 0xf;
  puVar1[-4] = 2;
  puVar1[-2] = 1;
  puVar1[2] = 1;
  uVar3 = FUN_a000137c();
  uVar2 = (undefined4)((ulonglong)uVar3 >> 0x20);
  if ((int)uVar3 != 0) {
    FUN_a000134c(param_1,1);
    uVar2 = 0;
    *DAT_a0001538 = 1;
  }
  return uVar2;
}



/* FUN_a000153c @ a000153c */

void FUN_a000153c(int param_1,uint param_2,undefined4 param_3,undefined4 param_4)

{
  byte bVar1;
  undefined4 uStack_c;
  
  uStack_c = param_2;
  FUN_a00014f0((int)&uStack_c + 3,0xb0,param_3,param_4,param_1);
  bVar1 = (byte)(uStack_c >> 0x18);
  if (param_1 == 0) {
    if (-1 < (int)((uStack_c >> 0x18) << 0x1f)) {
      return;
    }
    bVar1 = bVar1 & 0xfe;
  }
  else {
    if ((int)((uStack_c >> 0x18) << 0x1f) < 0) {
      return;
    }
    bVar1 = bVar1 | 1;
  }
  uStack_c = CONCAT13(bVar1,(undefined3)uStack_c);
  FUN_a00014a8((int)&uStack_c + 3,0xb0);
  return;
}



/* FUN_a0001574 @ a0001574 */

void FUN_a0001574(int param_1,uint param_2,undefined4 param_3,undefined4 param_4)

{
  byte bVar1;
  undefined4 uStack_c;
  
  uStack_c = param_2;
  FUN_a00014f0((int)&uStack_c + 3,0xb0,param_3,param_4,param_1);
  bVar1 = (byte)(uStack_c >> 0x18);
  if (param_1 == 0) {
    if ((int)((uStack_c >> 0x18) << 0x1c) < 0) {
      return;
    }
    bVar1 = bVar1 | 8;
  }
  else {
    if (-1 < (int)((uStack_c >> 0x18) << 0x1c)) {
      return;
    }
    bVar1 = bVar1 & 0xf7;
  }
  uStack_c = CONCAT13(bVar1,(undefined3)uStack_c);
  FUN_a00014a8((int)&uStack_c + 3,0xb0);
  return;
}



/* FUN_a00015ac @ a00015ac */

byte FUN_a00015ac(undefined4 param_1,uint3 param_2)

{
  char cVar1;
  short sVar2;
  undefined4 uStack_c;
  
  sVar2 = 10000;
  uStack_c = (uint)param_2;
  do {
    cVar1 = FUN_a00013f8((int)&uStack_c + 3);
    if (cVar1 == '\0' && (uStack_c & 0x1000000) == 0) {
      if ((uStack_c & 0x20000000) == 0) {
        return uStack_c._3_1_ & 0x20;
      }
      return 0xb;
    }
    sVar2 = sVar2 + -1;
  } while (sVar2 != 0);
  return 2;
}



/* FUN_a00015e8 @ a00015e8 */

char FUN_a00015e8(uint param_1)

{
  char cVar1;
  
  cVar1 = '\0';
  while( true ) {
    if (param_1 == 0) break;
    param_1 = param_1 >> 1;
    cVar1 = cVar1 + '\x01';
  }
  return cVar1 + -1;
}



/* FUN_a00015fc @ a00015fc */

void FUN_a00015fc(undefined4 param_1,int param_2)

{
  undefined1 *puVar1;
  int iVar2;
  undefined1 *puVar3;
  undefined1 *puVar4;
  undefined1 *puVar5;
  uint uVar6;
  undefined4 in_cr14;
  
  if (*DAT_a0001650 < 0x19) {
    InstructionSynchronizationBarrier(0xf);
    uVar6 = coprocessor_movefromRt(0xf,0,in_cr14);
    coprocessor_movefromRt2(0xf,0,in_cr14);
    iVar2 = *DAT_a0001650 * 0x28;
    puVar1 = (undefined1 *)(param_2 + -1);
    *(uint *)(iVar2 + -0x5fff0ffc) = (uint)((ulonglong)uVar6 * (ulonglong)DAT_a0001654 >> 0x22);
    *(undefined4 *)(iVar2 + -0x5fff0ff8) = param_1;
    puVar5 = (undefined1 *)(DAT_a0001658 + iVar2);
    puVar3 = puVar5;
    do {
      puVar1 = puVar1 + 1;
      puVar4 = puVar3 + 1;
      *puVar3 = *puVar1;
      puVar3 = puVar4;
    } while (puVar4 != puVar5 + 7);
    puVar5[7] = 0;
    *DAT_a0001650 = *DAT_a0001650 + 1;
  }
  return;
}



/* FUN_a000165c @ a000165c */

void FUN_a000165c(void)

{
  undefined2 *puVar1;
  
  puVar1 = DAT_a0001670;
  *DAT_a0001670 = 0;
  *puVar1 = 1;
  do {
  } while (-1 < (int)((uint)*DAT_a0001674 << 0x13));
  return;
}



/* uart_putc @ a0001678 */

void uart_putc(char c)
{
  while ((UART0_LSR & UART_LSR_THRE) == 0) {
    /* wait for the tx holding register to drain */
  }
  UART0_THR = c;
}



/* uart_put_hex8 @ a0001690 */

void uart_put_hex8(uint byte)
{
  uint hi = (byte >> 4) & 0xf;
  uint lo = byte & 0xf;

  uart_putc(hi < 10 ? hi + '0' : hi + ('a' - 10));
  uart_putc(lo < 10 ? lo + '0' : lo + ('a' - 10));
}



/* uart_put_hex32 @ a00016b6 */

void uart_put_hex32(uint val)
{
  uart_put_hex8((val >> 24) & 0xff);
  uart_put_hex8((val >> 16) & 0xff);
  uart_put_hex8((val >> 8) & 0xff);
  uart_put_hex8(val & 0xff);
}



/* uart_put_hex16 @ a00016da */

void uart_put_hex16(uint val)
{
  uart_put_hex8((val >> 8) & 0xff);
  uart_put_hex8(val & 0xff);
}



/* uart_puts @ a00016ee */

void uart_puts(const char *s)
{
  while (*s != '\0') {
    uart_putc(*s++);
  }
}



/* FUN_a0001700 @ a0001700 */

void FUN_a0001700(ushort *param_1,short *param_2,undefined1 *param_3)

{
  undefined4 uVar1;
  int iVar2;
  undefined1 uVar3;
  short sVar4;
  uint uVar5;
  byte *pbVar6;
  byte *pbVar7;
  
  if (*(ushort *)(DAT_a00017c8 + 3) < 0x3f) {
    for (uVar5 = 0; uVar5 < *(ushort *)(DAT_a00017c8 + 3); uVar5 = uVar5 + 1) {
      if (*(short *)(DAT_a00017cc + uVar5 * 8 + 4) == 1) goto LAB_a0001734;
    }
  }
  uart_puts(DAT_a00017d0);
  uart_put_hex16(1);
  uart_puts(DAT_a00017d4);
  uart_puts(DAT_a00017d8);
  uVar5 = 1;
LAB_a0001734:
  uVar1 = DAT_a00017e8;
  iVar2 = DAT_a00017dc;
  if (*(byte *)(DAT_a00017dc + 0x20) == 0) {
    iVar2 = 0;
    pbVar7 = DAT_a00017e0;
    do {
      pbVar6 = pbVar7 + 1;
      iVar2 = iVar2 + (uint)*pbVar7;
      pbVar7 = pbVar6;
    } while (pbVar6 != DAT_a00017e4);
    if (*DAT_a00017c8 == iVar2) {
      if ((uint)*(ushort *)(DAT_a00017c8 + 3) < (uVar5 & 0xffff)) {
        *param_1 = 0;
        uart_puts(uVar1);
        uart_puts(DAT_a00017ec);
        uart_puts(DAT_a00017f0);
        uart_put_hex16(1);
        uart_puts(DAT_a00017d4);
        uart_puts(DAT_a00017f4);
        return;
      }
      iVar2 = uVar5 * 8;
      *param_1 = *(ushort *)(iVar2 + -0x5fff55f0);
      *param_2 = *(short *)(&DAT_a000aa18 + iVar2);
      uVar3 = (undefined1)*(undefined2 *)(iVar2 + -0x5fff55ea);
      goto LAB_a000178a;
    }
    *param_1 = 0x10;
    sVar4 = 0x13;
  }
  else {
    *param_1 = (ushort)*(byte *)(DAT_a00017dc + 0x20);
    sVar4 = *(byte *)(iVar2 + 0x20) + 2;
  }
  *param_2 = sVar4;
  uVar3 = 1;
LAB_a000178a:
  *param_3 = uVar3;
  return;
}



/* FUN_a00017f8 @ a00017f8 */

void FUN_a00017f8(void)

{
  FUN_a00002ec();
  uart_puts(DAT_a000181c);
  uart_puts(DAT_a0001820);
  do {
                    /* WARNING: Do nothing block with infinite loop */
  } while( true );
}



/* FUN_a0001824 @ a0001824 */

void FUN_a0001824(uint param_1,undefined4 param_2,undefined4 param_3,undefined2 param_4)

{
  bool bVar1;
  uint uVar2;
  int iVar3;
  uint uVar4;
  int iVar5;
  undefined4 uStack_28;
  undefined4 uStack_24;
  undefined4 uStack_20;
  undefined2 uStack_1c;
  undefined2 local_1a;
  
  _uStack_1c = CONCAT22(0x30,param_4);
  uVar2 = param_1;
  iVar3 = 0xb;
  uStack_24 = param_2;
  uStack_20 = param_3;
  do {
    iVar5 = iVar3;
    iVar3 = DAT_a000186c;
    if (iVar5 == 0) goto LAB_a000185e;
    uVar4 = (uint)((ulonglong)uVar2 * (ulonglong)DAT_a0001868 >> 0x23);
    *(char *)((int)&uStack_28 + iVar5 + 3) = (char)uVar2 + '0' + (char)uVar4 * -10;
    bVar1 = 9 < uVar2;
    uVar2 = uVar4;
    iVar3 = iVar5 + -1;
  } while (bVar1);
  iVar3 = (int)&uStack_28 + iVar5 + 3;
LAB_a000185e:
  uStack_28 = param_1;
  uart_puts(iVar3);
  return;
}



/* FUN_a0001870 @ a0001870 */

undefined4 FUN_a0001870(uint param_1)

{
  undefined2 *puVar1;
  ushort *puVar2;
  undefined4 uVar3;
  undefined8 uVar4;
  
  puVar1 = DAT_a00018cc;
  *DAT_a00018cc = 7;
  puVar2 = DAT_a00018d0;
  puVar1[0x12] = 0;
  puVar1[0x14] = 0;
  *puVar2 = (byte)(param_1 >> 0x10) | 0x13;
  puVar2 = DAT_a00018d4;
  *DAT_a00018d4 = (ushort)((param_1 & 0xff) << 8) | (ushort)(param_1 >> 8) & 0xff;
  puVar2[0x12] = 4;
  puVar2[0x14] = 0;
  puVar2[0x18] = 1;
  uVar4 = FUN_a000137c();
  uVar3 = (undefined4)((ulonglong)uVar4 >> 0x20);
  if ((int)uVar4 == 0) {
    uart_puts(DAT_a00018d8);
    uart_puts(DAT_a00018dc);
    FUN_a0001824(0x188);
    uart_puts(DAT_a00018e0);
    uart_puts(DAT_a00018e4);
    uVar3 = 2;
  }
  else {
    *DAT_a00018e8 = 1;
  }
  return uVar3;
}



/* FUN_a00018ec @ a00018ec */

undefined4 FUN_a00018ec(byte *param_1,undefined4 param_2,undefined4 param_3)

{
  undefined2 *puVar1;
  undefined2 *puVar2;
  undefined2 *puVar3;
  undefined2 *puVar4;
  short sVar5;
  undefined6 uVar6;
  byte local_21;
  undefined4 uStack_20;
  
  puVar4 = DAT_a0001998;
  puVar3 = DAT_a0001994;
  puVar2 = DAT_a0001990;
  sVar5 = 0x65;
  local_21 = 1;
  uStack_20 = param_3;
  while (((int)((uint)local_21 << 0x1f) < 0 && (sVar5 = sVar5 + -1, sVar5 != 0))) {
    *puVar2 = 7;
    *puVar3 = 0;
    *puVar4 = 0;
    puVar1 = DAT_a0001978;
    *DAT_a0001978 = 0xc00f;
    puVar1[0x14] = 2;
    puVar1[0x16] = 1;
    puVar1[0x1a] = 1;
    uVar6 = FUN_a000137c();
    if ((int)uVar6 == 0) {
      uart_puts(DAT_a000197c);
      uart_puts(DAT_a0001980);
      FUN_a0001824(0x149);
      uart_puts(DAT_a0001984);
      uart_puts(DAT_a0001988);
      return 2;
    }
    *DAT_a000198c = (short)((uint6)uVar6 >> 0x20);
    FUN_a000134c(&local_21);
  }
  *param_1 = local_21;
  return 0;
}



/* FUN_a000199c @ a000199c */

undefined4 FUN_a000199c(undefined2 param_1)

{
  undefined2 *puVar1;
  undefined4 uVar2;
  undefined8 uVar3;
  
  puVar1 = DAT_a00019ec;
  *DAT_a00019ec = 7;
  puVar1[0x12] = 0;
  puVar1[0x14] = 0;
  puVar1[-0x18] = param_1;
  puVar1[-4] = 1;
  puVar1[-2] = 0;
  puVar1[2] = 1;
  uVar3 = FUN_a000137c();
  uVar2 = (undefined4)((ulonglong)uVar3 >> 0x20);
  if ((int)uVar3 == 0) {
    uart_puts(DAT_a00019f0);
    uart_puts(DAT_a00019f4);
    FUN_a0001824(0xdc);
    uart_puts(DAT_a00019f8);
    uart_puts(DAT_a00019fc);
    uVar2 = 2;
  }
  else {
    *DAT_a0001a00 = 1;
  }
  return uVar2;
}



/* FUN_a0001a04 @ a0001a04 */

int FUN_a0001a04(void)

{
  return DAT_a0001a18[1] * 0x10000 + (*DAT_a0001a18 & 0xffff);
}



/* FUN_a0001a1c @ a0001a1c */

undefined4 FUN_a0001a1c(void)

{
  ushort *puVar1;
  int iVar2;
  undefined4 uVar3;
  uint uVar4;
  undefined4 extraout_r2;
  undefined8 uVar5;
  
  iVar2 = FUN_a0001a04();
  puVar1 = DAT_a0001a4c;
  uVar4 = DAT_a0001a50;
  do {
    if ((*puVar1 & 1) != 0) {
      uVar3 = 1;
      break;
    }
    uVar5 = FUN_a0001a04(*puVar1,uVar4);
    uVar4 = (uint)((ulonglong)uVar5 >> 0x20);
    uVar3 = extraout_r2;
  } while ((uint)((int)uVar5 - iVar2) <= uVar4);
  *DAT_a0001a54 = *DAT_a0001a54 | 1;
  return uVar3;
}



/* FUN_a0001a58 @ a0001a58 */

uint FUN_a0001a58(byte param_1)

{
  ushort *puVar1;
  uint uVar2;
  ushort *puVar3;
  
  *DAT_a0001b10 = 1;
  puVar1 = DAT_a0001b14;
  puVar3 = DAT_a0001b14 + 2;
  *DAT_a0001b14 = *DAT_a0001b14 & 0xff00 | 6;
  *puVar1 = *puVar1 & 0xff | 0x3100;
  *puVar3 = *puVar3 & 0xff00 | (ushort)param_1;
  *puVar3 = *puVar3 & 0xff | 0x500;
  puVar1[0x14] = puVar1[0x14] & 0xfff0 | 1;
  puVar1[0x14] = puVar1[0x14] & 0xff0f | 0x20;
  puVar1[0x14] = puVar1[0x14] & 0xf0ff | 0x100;
  puVar1[0x16] = puVar1[0x16] & 0xfff0;
  puVar1[0x16] = puVar1[0x16] & 0xff0f;
  puVar1[0x16] = puVar1[0x16] & 0xf0ff | 0x100;
  *DAT_a0001b18 = 0xf007;
  *DAT_a0001b1c = *DAT_a0001b1c | 1;
  uVar2 = FUN_a0001a1c();
  if ((uVar2 & 1) == 0) {
    uart_puts(DAT_a0001b20);
  }
  return uVar2 & 1;
}



/* FUN_a0001b24 @ a0001b24 */

uint FUN_a0001b24(ushort param_1,ushort *param_2)

{
  ushort *puVar1;
  uint uVar2;
  
  *DAT_a0001bb4 = 1;
  puVar1 = DAT_a0001bb8;
  *DAT_a0001bb8 = param_1 | *DAT_a0001bb8 & 0xff00;
  puVar1[0x14] = puVar1[0x14] & 0xfff0 | 1;
  puVar1[0x14] = puVar1[0x14] & 0xff0f;
  puVar1[0x14] = puVar1[0x14] & 0xf0ff;
  puVar1[0x16] = puVar1[0x16] & 0xfff0 | 1;
  puVar1[0x16] = puVar1[0x16] & 0xff0f;
  puVar1[0x16] = puVar1[0x16] & 0xf0ff;
  *DAT_a0001bbc = 7;
  *DAT_a0001bc0 = *DAT_a0001bc0 | 1;
  uVar2 = FUN_a0001a1c();
  if ((uVar2 & 1) == 0) {
    uart_puts(DAT_a0001bc4);
  }
  *param_2 = *DAT_a0001bc8 & 0xff;
  return uVar2 & 1;
}



/* FUN_a0001bcc @ a0001bcc */

uint FUN_a0001bcc(ushort param_1)

{
  ushort *puVar1;
  uint uVar2;
  ushort *puVar3;
  
  *DAT_a0001cc0 = 1;
  puVar1 = DAT_a0001cc4;
  puVar3 = DAT_a0001cc4 + 2;
  *DAT_a0001cc4 = *DAT_a0001cc4 & 0xff00 | 6;
  *puVar1 = *puVar1 & 0xff | 0x100;
  *puVar3 = param_1 | *puVar3 & 0xff00;
  *puVar3 = *puVar3 & 0xff;
  puVar1[4] = puVar1[4] & 0xff00 | 5;
  puVar1[0x14] = puVar1[0x14] & 0xfff0 | 1;
  puVar1[0x14] = puVar1[0x14] & 0xff0f | 0x30;
  puVar1[0x14] = puVar1[0x14] & 0xf0ff | 0x100;
  puVar1[0x16] = puVar1[0x16] & 0xfff0;
  puVar1[0x16] = puVar1[0x16] & 0xff0f;
  puVar1[0x16] = puVar1[0x16] & 0xf0ff | 0x100;
  puVar1 = DAT_a0001cc8;
  *DAT_a0001cc8 = *DAT_a0001cc8 | 1;
  *puVar1 = *puVar1 | 2;
  *puVar1 = *puVar1 | 4;
  *puVar1 = *puVar1 | 0x8000;
  *puVar1 = *puVar1 | 0x4000;
  *puVar1 = *puVar1 & 0xe7ff | 0x1000;
  *puVar1 = *puVar1 | 0x2000;
  *DAT_a0001ccc = *DAT_a0001ccc | 1;
  uVar2 = FUN_a0001a1c();
  if ((uVar2 & 1) == 0) {
    uart_puts(DAT_a0001cd0);
  }
  return uVar2 & 1;
}



/* FUN_a0001cd4 @ a0001cd4 */

void FUN_a0001cd4(uint param_1)

{
  uint *puVar1;
  int *piVar2;
  uint uVar3;
  uint uVar4;
  int extraout_r2;
  int iVar5;
  undefined8 uVar6;
  
  uVar3 = FUN_a0001a04();
  piVar2 = DAT_a0001d0c;
  puVar1 = DAT_a0001d08;
  uVar4 = 0;
  do {
    uVar6 = FUN_a0001a04(uVar3,uVar3,uVar4);
    uVar4 = (uint)((ulonglong)uVar6 >> 0x20);
    uVar3 = (uint)uVar6;
    iVar5 = extraout_r2;
    if (uVar3 < uVar4) {
      iVar5 = extraout_r2 + *piVar2 * 0x10000 + (*puVar1 & 0xffff);
    }
    uVar4 = (iVar5 + uVar3) - uVar4;
  } while (uVar4 < param_1);
  return;
}



/* FUN_a0001d10 @ a0001d10 */

void FUN_a0001d10(void)

{
  byte *pbVar1;
  undefined4 uVar2;
  
  uart_puts(DAT_a0001d3c);
  uVar2 = DAT_a0001d44;
  pbVar1 = DAT_a0001d40;
  *DAT_a0001d40 = 0x10;
  *pbVar1 = *pbVar1 | 0x40;
  uart_puts(uVar2);
  uart_puts(DAT_a0001d48);
  *DAT_a0001d4c = 0x1011;
  return;
}



/* FUN_a0001d50 @ a0001d50 */

/* WARNING: Removing unreachable block (ram,0xa0003afe) */
/* WARNING: Removing unreachable block (ram,0xa0003b20) */
/* WARNING: Removing unreachable block (ram,0xa0003aba) */
/* WARNING: Removing unreachable block (ram,0xa0003ac2) */
/* WARNING: Removing unreachable block (ram,0xa0003a98) */
/* WARNING: Removing unreachable block (ram,0xa0003aa0) */
/* WARNING: Removing unreachable block (ram,0xa0003adc) */
/* WARNING: Removing unreachable block (ram,0xa0003ae4) */
/* WARNING: Removing unreachable block (ram,0xa0003af8) */
/* WARNING: Removing unreachable block (ram,0xa0003b06) */
/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void FUN_a0001d50(void)

{
  byte bVar1;
  byte bVar2;
  ushort uVar3;
  ulonglong uVar4;
  char cVar5;
  char cVar6;
  undefined1 *puVar7;
  undefined2 *puVar8;
  byte *pbVar9;
  byte *pbVar10;
  undefined1 *puVar11;
  undefined2 *puVar12;
  ushort *puVar13;
  undefined1 *puVar14;
  ushort *puVar15;
  ushort *puVar16;
  undefined4 uVar17;
  ushort *puVar18;
  ushort *puVar19;
  short *psVar20;
  int *piVar21;
  undefined1 uVar22;
  undefined2 uVar23;
  uint uVar24;
  int iVar25;
  undefined2 *puVar26;
  uint uVar27;
  uint uVar28;
  uint uVar29;
  int iVar30;
  undefined4 uVar31;
  undefined4 uVar32;
  int extraout_r1;
  int extraout_r1_00;
  ushort uVar33;
  uint uVar34;
  undefined2 *puVar35;
  ushort *puVar36;
  ushort uVar37;
  byte *pbVar38;
  undefined2 *puVar39;
  undefined2 *puVar40;
  undefined2 *puVar41;
  uint extraout_r3;
  char *pcVar42;
  ushort uVar43;
  undefined1 *puVar44;
  byte *pbVar45;
  undefined1 *puVar46;
  uint uVar47;
  undefined4 *puVar48;
  int iVar49;
  undefined4 in_cr14;
  undefined8 uVar50;
  undefined8 uVar51;
  uint local_84;
  undefined2 *local_7c;
  byte local_68;
  byte local_67;
  undefined2 local_66;
  byte local_64 [2];
  undefined2 uStack_62;
  undefined4 uStack_60;
  undefined1 local_5c;
  undefined4 local_50;
  undefined4 uStack_4c;
  undefined4 uStack_48;
  undefined4 uStack_44;
  undefined4 local_40;
  undefined4 uStack_3c;
  undefined4 uStack_38;
  undefined4 uStack_34;
  undefined4 local_30;
  undefined4 uStack_2c;
  undefined4 uStack_28;
  undefined4 uStack_24;
  
  bVar1 = *DAT_a0001f44;
  *DAT_a0001f48 = 0;
  uVar24 = FUN_a0001a04();
  FUN_a00015fc((uint)((ulonglong)uVar24 * (ulonglong)DAT_a0001f4c >> 0x23),DAT_a0001f50,
               (int)((ulonglong)uVar24 * (ulonglong)DAT_a0001f4c));
  puVar36 = DAT_a0001f54;
  *DAT_a0001f54 = *DAT_a0001f54 | 1;
  puVar36[0x1cd8] = puVar36[0x1cd8] & 0xff8f | 0x10;
  puVar36[0x1ce2] = puVar36[0x1ce2] & 0xfcff | 0x100;
  iVar25 = FUN_a000119c();
  if ((iVar25 == 0) || (uVar24 = FUN_a00011c8(0), (uVar24 & 0x7ffff) >> 0x10 == 2)) {
    *DAT_a0001f58 = *DAT_a0001f58 & 0x3f;
  }
  uVar24 = 0;
  *DAT_a0001f5c = 0;
  FUN_a0001cd4(0x4b0);
  *DAT_a0001f60 = 0;
  FUN_a0001cd4(12000);
  pbVar38 = DAT_a0001f68;
  puVar44 = DAT_a0001f64;
  *DAT_a0001f64 = 0x30;
  *DAT_a0001f6c = 0x23;
  puVar7 = DAT_a0001f70;
  *DAT_a0001f70 = 0;
  *pbVar38 = *pbVar38 | 0x10;
  pbVar38 = DAT_a0001f7c;
  puVar36 = DAT_a0001f78;
  do {
    uVar34 = uVar24 & 0xff;
    if ((int)((uint)*DAT_a0001f74 << 0x19) < 0) goto LAB_a0001e08;
    uVar24 = uVar24 + 1;
  } while (uVar24 != 0x100);
  uVar34 = 0xff;
LAB_a0001e08:
  *puVar7 = 0;
  pbVar10 = DAT_a0001fb4;
  puVar8 = DAT_a0001f80;
  *puVar36 = *puVar36 & 0xf7ff;
  *puVar8 = 0x3210;
  *pbVar38 = *pbVar38 & 0xfe;
  *pbVar38 = *pbVar38 | 1;
  pbVar38[-0x68] = 0;
  pbVar45 = DAT_a0001f84;
  *DAT_a0001f84 = *DAT_a0001f84 | 0x10;
  pbVar9 = DAT_a0001f8c;
  while (bVar2 = *pbVar10, (bVar2 & 1) != 0) {
    if (uVar34 == 0xff) goto LAB_a0001e64;
    uVar34 = uVar34 + 1 & 0xff;
  }
  if (uVar34 == 0xff) {
LAB_a0001e64:
    *DAT_a0001f88 = 0xbf1;
  }
  else {
    *DAT_a0001f8c = *DAT_a0001f8c | 0x80;
    *DAT_a0001fb0 = 0x5e;
    pbVar38[-0x68] = bVar2 & 1;
    *pbVar9 = *pbVar9 & 0x7f;
  }
  puVar48 = DAT_a0001f90;
  uVar50 = 0;
  *DAT_a0001f8c = 3;
  *pbVar45 = *pbVar45 & 0xef;
  *DAT_a0001f94 = 7;
  *puVar8 = 0x3012;
  *puVar36 = *puVar36 | 0x800;
  local_50 = *puVar48;
  uStack_4c = puVar48[1];
  uStack_48 = puVar48[2];
  local_64[0] = 0;
  uStack_44 = puVar48[3];
  local_40 = puVar48[4];
  uStack_3c = puVar48[5];
  uStack_38 = puVar48[6];
  uStack_34 = puVar48[7];
  local_30 = puVar48[8];
  uStack_2c = puVar48[9];
  uStack_28 = puVar48[10];
  uStack_24 = puVar48[0xb];
  local_64[1] = 0;
  uStack_62 = 0;
  uStack_60 = 0;
  local_84 = 8;
  pbVar38 = local_64;
  pbVar45 = (byte *)((int)&uStack_48 + 1);
  do {
    *pbVar38 = *pbVar45;
    local_84 = local_84 + -1;
    pbVar38 = pbVar38 + 1;
    pbVar45 = pbVar45 + 1;
  } while (local_84 != 0);
  local_5c = 0;
  uart_puts(DAT_a0001f98);
  uart_puts(local_64);
  uart_puts(DAT_a0001f9c);
  uart_puts(DAT_a0001fa0);
  uart_put_hex8(bVar1);
  uart_puts(DAT_a0001fa4);
  puVar36 = DAT_a000232c;
  uVar32 = DAT_a0001fac;
  if (((int)((uint)*DAT_a0001fa8 << 0x1e) < 0) &&
     (uVar32 = DAT_a0002470, (int)((uint)*DAT_a000232c << 0x1f) < 0)) {
    uart_puts(DAT_a0002330);
    *puVar36 = *puVar36 | 1;
  }
  else {
    uart_puts(uVar32);
  }
  FUN_a00015fc(0x29e,DAT_a0002334);
  if (bVar1 == 0x1d) {
    uart_puts(DAT_a00027ec);
    puVar12 = DAT_a0002828;
    puVar8 = DAT_a00027f4;
    puVar11 = DAT_a00027f0;
    *DAT_a00027f0 = 0;
    puVar11[3] = 0;
    puVar11[4] = 0;
    puVar11[7] = 0x1c;
    puVar11[8] = 2;
    puVar11[0xb] = 0x10;
    puVar35 = DAT_a00027f8;
    puVar11[0xc] = 0;
    *puVar8 = 0xc00;
    *puVar8 = 0xc00;
    *puVar8 = 0xc00;
    *puVar8 = 0xc01;
    *puVar12 = 0xfffe;
    *puVar35 = 0xffff;
    puVar35[0x20] = 0xffff;
    puVar35[0x40] = 0xffff;
    puVar35[-0x160] = 0xffff;
    puVar35[-0x140] = 0xffff;
    puVar35[-0x80] = 0xfffe;
    puVar40 = DAT_a0002830;
    puVar35 = DAT_a000282c;
    *DAT_a00027fc = 1;
    FUN_a0001cd4(12000);
    *puVar35 = 0x1000;
    FUN_a0001cd4(12000);
    *puVar35 = 0;
    FUN_a0001cd4(12000);
    puVar35 = DAT_a0002800;
    *DAT_a0002800 = 0x400;
    puVar35[-2] = 0x2004;
    *puVar40 = 1;
    puVar35[-6] = 0x8000;
    puVar35[-4] = 0x29;
    FUN_a0001cd4(12000);
    puVar39 = DAT_a0002808;
    puVar35 = DAT_a0002804;
    *DAT_a0002804 = 4;
    puVar35[10] = 0x114;
    puVar35[0x66] = 0x11;
    puVar35[0x1e0] = 0x292;
    puVar35[0x1e2] = 0x52;
    puVar35[0x1e4] = 0x1b50;
    puVar35[0x1e6] = 0x1e99;
    puVar35[0x1e8] = 0x2777;
    puVar35[0x1ea] = 0x95a8;
    puVar35[0x1ec] = 0x404c;
    puVar35[0x1ee] = 0x203;
    puVar35[0x1f0] = 0x4004;
    puVar35[0x1f2] = 0x8000;
    puVar35[500] = 0xc000;
    puVar35[0x206] = 0x70;
    puVar35[0x2b0] = 0x6000;
    puVar35[0x300] = 3;
    puVar35[0x31c] = 0;
    puVar35[0x31e] = 0x909;
    puVar35[800] = 0x71e;
    puVar35[0x322] = 0x2707;
    puVar35[0x324] = 0x908;
    puVar35[0x326] = 0x905;
    puVar35[0x328] = 0x304;
    puVar35[0x32a] = 0x528;
    puVar35[0x32c] = 0x46;
    puVar35[0x32e] = 0xe000;
    puVar35[0x330] = 0;
    puVar35[0x332] = 0x900;
    puVar35[0x35e] = 0;
    puVar35[0x364] = 0;
    puVar35[0x3dc] = 0;
    puVar35[0x13e] = 0;
    puVar35[0x140] = 0;
    puVar35[0x142] = 0;
    puVar35[0x144] = 0x30;
    puVar35[0x146] = 0x5000;
    puVar35[-0x20] = 0xaaaa;
    puVar35[-0x1e] = 0;
    puVar35[-0x18] = 0x1100;
    puVar35[-0x14] = 0x8f;
    puVar35[0xc] = 0x1122;
    puVar35[0x16] = 0x77;
    puVar35[0x18] = 0x5050;
    puVar35[0x1a] = 0x9111;
    *puVar39 = 0x1111;
    puVar39[10] = 0x77;
    puVar39[0xc] = 0;
    puVar39[0xe] = 0x11;
    puVar39[0x10] = 0x11;
    puVar35 = DAT_a000280c;
    *DAT_a000280c = 0x1111;
    puVar35[2] = 0;
    puVar39[0x2e] = 0x808;
    puVar39[0x30] = 0x808;
    puVar39[0x36] = 0x404;
    puVar39[0x38] = 0x404;
    puVar35 = DAT_a0002810;
    puVar26 = DAT_a0002810 + 0xe;
    *DAT_a0002810 = 0x1317;
    puVar35[0xc] = 0x6466;
    *puVar26 = 0x6666;
    puVar35[0x10] = 0x1112;
    puVar35[0x12] = 0x4112;
    puVar35[0x14] = 0x1111;
    puVar35[0x16] = 0x1111;
    puVar35[0x18] = 0x1111;
    puVar35[0x1a] = 0x1111;
    puVar35[0x22] = 0;
    puVar35[0x24] = 0x1111;
    puVar35[0x26] = 0x111;
    puVar35[0x28] = 0x111;
    puVar35[0x2a] = 0x111;
    puVar35[0x3c] = 0x4444;
    puVar35[0x3e] = 0x4444;
    puVar35[0x40] = 0x4444;
    puVar35[0x42] = 0x4444;
    *DAT_a0002814 = 0x44;
    puVar35 = DAT_a0002818;
    *DAT_a0002818 = 0x5555;
    puVar35[2] = 0x5555;
    puVar35[4] = 0x5555;
    puVar35[6] = 0x5555;
    puVar35 = DAT_a000281c;
    puVar26 = DAT_a000281c + -0x86;
    *DAT_a000281c = 0x55;
    puVar41 = puVar35 + -0x88;
    *puVar26 = 0x7f;
    puVar35[-0x84] = 0xf000;
    *puVar41 = 0xcb;
    *puVar41 = 0xcf;
    *puVar41 = 0xcb;
    *puVar41 = 0xc3;
    *puVar41 = 0xcb;
    *puVar41 = 0xc3;
    *puVar41 = 0xcb;
    *puVar41 = 0xc2;
    *puVar41 = 0xc0;
    *puVar41 = 0x33c8;
    puVar26 = DAT_a0002820;
    puVar35[-0x78] = 0;
    *puVar26 = 0;
    puVar26[2] = 0;
    puVar26[-8] = 0xf0f1;
    puVar35[-0x78] = 0x800;
    puVar39[0x1ee] = 0x8021;
    puVar39[0x2be] = 0x951a;
    puVar39[0x214] = 0xffff;
    puVar39[0x234] = 0xffff;
    puVar39[0x254] = 0xffff;
    puVar39[0x274] = 0xffff;
    puVar39[0xd4] = 0xffff;
    puVar39[0xf4] = 0xffff;
    puVar39[0x202] = 0x8015;
    puVar39[0x222] = 0x8015;
    puVar39[0x242] = 0x8015;
    puVar39[0x262] = 0x8015;
    puVar39[0xc2] = 0x8015;
    puVar39[0xe2] = 0x8015;
    puVar39 = DAT_a0002824;
    *puVar40 = 1;
    puVar35[-0x78] = 0x800;
    puVar35[-0x90] = 0xa0a;
    puVar35[-0x8e] = 0xaaaa;
    puVar35[-0x8c] = 0xaaaa;
    puVar35[-0x8a] = 0xaaaa;
    puVar35[-0xce] = 0x8000;
    puVar35[-0xcc] = 0x20;
    puVar35[-0xe0] = 0x3f;
    *puVar39 = 5;
    *puVar39 = 0xf;
    *puVar39 = 5;
    *puVar8 = 0x8c01;
    *puVar8 = 0x8c00;
    puVar35 = DAT_a0002bbc;
    *DAT_a0002bbc = 0x2010;
    *puVar35 = 0;
    _DAT_1f202030 = 0;
    _DAT_1f2020f8 = 0;
    _DAT_1f2020a8 = 0x4000;
    *puVar39 = 5;
    *puVar39 = 0xf;
    *puVar39 = 5;
    *puVar35 = 1;
    puVar8[-0x1e] = 0;
    FUN_a0001cd4(12000);
    puVar8[-0x1e] = 8;
    puVar8[-0x1e] = 0xc;
    FUN_a0001cd4(12000);
    puVar8[-0x1e] = 0xe;
    FUN_a0001cd4(12000);
    puVar8[-0x1e] = 0xf;
    FUN_a0001cd4(12000);
    FUN_a0001cd4(12000);
    FUN_a0001cd4(12000);
    *puVar39 = 5;
    *puVar39 = 0xf;
    *puVar39 = 5;
    *puVar12 = 0x7ffe;
LAB_a00028c0:
    puVar8 = DAT_a0002bc0;
    *DAT_a0002bc0 = 0xfffa;
    puVar8[0x118] = 0xa0e1;
    puVar8[0x118] = 0x80e1;
    *DAT_a0002bc4 = 0;
  }
  else {
    if ((0x1c < bVar1) && (bVar1 < 0x20)) {
      uart_puts(DAT_a0002338);
      puVar8 = DAT_a0002340;
      puVar11 = DAT_a000233c;
      *DAT_a000233c = 0;
      puVar11[3] = 0;
      puVar11[4] = 0;
      puVar26 = DAT_a0002370;
      puVar11[7] = 0x1e;
      puVar11[8] = 1;
      puVar11[0xb] = 0x10;
      puVar35 = DAT_a0002344;
      puVar11[0xc] = 0;
      *puVar8 = 0xc00;
      *puVar8 = 0xc00;
      *puVar8 = 0xc00;
      *puVar8 = 0xc01;
      *puVar26 = 0xfffe;
      *puVar35 = 0xffff;
      puVar35[0x20] = 0xffff;
      puVar35[0x40] = 0xffff;
      puVar35[-0x160] = 0xffff;
      puVar35[-0x140] = 0xffff;
      puVar35[-0x80] = 0xfffe;
      puVar12 = DAT_a0002374;
      *DAT_a0002348 = 1;
      FUN_a0001cd4(12000);
      *puVar12 = 0x1000;
      FUN_a0001cd4(12000);
      *puVar12 = 0;
      FUN_a0001cd4(12000);
      puVar35 = DAT_a000234c;
      *DAT_a000234c = 0x400;
      puVar35[-2] = 0x2004;
      puVar12[0x66] = 1;
      puVar35[-6] = 0x8f5c;
      puVar35[-4] = 0x1e;
      FUN_a0001cd4(12000);
      puVar35 = DAT_a0002350;
      *DAT_a0002350 = 4;
      puVar35[10] = 0x114;
      puVar35[0x66] = 0x11;
      puVar35[0x1e0] = 0x2a3;
      puVar35[0x1e2] = 0x54;
      puVar35[0x1e4] = 0x1570;
      puVar35[0x1e6] = 0x20dd;
      puVar35[0x1e8] = 0x2d76;
      puVar35[0x1ea] = 0xe7e9;
      puVar35[0x1ec] = 0x4096;
      puVar35[0x1ee] = 0x1f14;
      puVar35[0x1f0] = 0x4004;
      puVar35[0x1f2] = 0x8020;
      puVar35[500] = 0xc000;
      puVar35[0x206] = 0xb0;
      puVar35[0x300] = 3;
      puVar35[0x31e] = 0xd0d;
      puVar35[800] = 0x620;
      puVar35[0x322] = 0x2d07;
      puVar35[0x324] = 0xe09;
      puVar35[0x326] = 0xe07;
      puVar35[0x328] = 0x504;
      puVar35[0x32a] = 0x528;
      puVar35[0x32c] = 0x96;
      puVar35[0x32e] = 0xe000;
      puVar35[0x330] = 0;
      puVar35[0x332] = 0xd00;
      puVar35[0x35e] = 0;
      puVar35[0x364] = 0;
      puVar35[0x3dc] = 0;
      puVar35[0x13e] = 0;
      puVar35[0x140] = 0;
      puVar35[0x142] = 0;
      puVar35[0x144] = 0x30;
      puVar35[0x146] = 0x5000;
      puVar35[-0x20] = 0xaaaa;
      puVar35[-0x1e] = 0x80;
      puVar35[-0x18] = 0x2200;
      puVar35[-0x14] = 0x97;
      puVar35[0xc] = 0x1122;
      puVar39 = DAT_a0002378;
      _DAT_1f202070 = 0x77;
      _DAT_1f202074 = 0x6066;
      _DAT_1f202078 = 0x9422;
      _DAT_1f20207c = 0xa044;
      _DAT_1f202090 = 0x77;
      *DAT_a0002354 = 0x6060;
      puVar35 = DAT_a0002358;
      *DAT_a0002358 = 0x44;
      puVar35[2] = 0x44;
      puVar35[4] = 0x1111;
      puVar35[6] = 0xc;
      *puVar39 = 0x808;
      puVar39[2] = 0x808;
      puVar39[8] = 0x404;
      puVar39[10] = 0x404;
      puVar35 = DAT_a000235c;
      *DAT_a000235c = 0x1313;
      puVar35[0xc] = 0x4045;
      puVar35[0xe] = 0x5453;
      puVar35[0x10] = 0x6555;
      puVar35[0x12] = 0x6666;
      puVar35[0x14] = 0x1111;
      puVar39 = DAT_a000237c;
      puVar35[0x16] = 0x1111;
      puVar35[0x18] = 0x1111;
      puVar35[0x1a] = 0x1111;
      *DAT_a0002360 = 0;
      puVar35[0x24] = 0x4444;
      *puVar39 = 0x444;
      puVar39[2] = 0x444;
      puVar39[4] = 0x444;
      puVar35 = DAT_a0002364;
      *DAT_a0002364 = 0x4444;
      puVar35[2] = 0x4444;
      puVar35[4] = 0x5555;
      puVar35[6] = 0x5555;
      puVar35[8] = 0x54;
      puVar35[0x10] = 0x5555;
      puVar35[0x12] = 0x5555;
      puVar35[0x14] = 0x5555;
      puVar35[0x16] = 0x5555;
      puVar35 = DAT_a0002368;
      puVar39 = DAT_a0002368 + -0x86;
      *DAT_a0002368 = 0x55;
      puVar40 = puVar35 + -0x88;
      *puVar39 = 0x7f;
      puVar35[-0x84] = 0xf000;
      *puVar40 = 0xcb;
      *puVar40 = 0xcf;
      *puVar40 = 0xcb;
      *puVar40 = 0xc3;
      *puVar40 = 0xcb;
      *puVar40 = 0xc3;
      *puVar40 = 0xcb;
      *puVar40 = 0xc2;
      *puVar40 = 0xc0;
      *puVar40 = 0x33c8;
      puVar39 = DAT_a000236c;
      puVar35[-0x78] = 0;
      *puVar39 = 0;
      puVar39[2] = 0;
      puVar39[-8] = 0xf0f1;
      puVar39 = DAT_a0002380;
      puVar35[-0x78] = 0x800;
      *puVar39 = 0x8021;
      puVar39[0xd0] = 0x951a;
      puVar39[0x26] = 0xffff;
      puVar39[0x46] = 0xffff;
      puVar39[0x66] = 0xffff;
      puVar39[0x86] = 0xffff;
      puVar39[-0x11a] = 0xffff;
      puVar39[-0xfa] = 0xffff;
      puVar39[0x14] = 0x8015;
      puVar39[0x34] = 0x8015;
      puVar39[0x54] = 0x8015;
      puVar39[0x74] = 0x8015;
      puVar39[-300] = 0x8015;
      puVar39[-0x10c] = 0x8015;
      puVar39 = DAT_a0002468;
      puVar12[0x66] = 1;
      puVar35[-0x78] = 0x800;
      puVar35[-0x90] = 0xa0a;
      puVar35[-0x8e] = 0xaaaa;
      puVar35[-0x8c] = 0xaaaa;
      puVar35[-0x8a] = 0xaaaa;
      puVar35[-0xce] = 0x8000;
      puVar35[-0xcc] = 0x20;
      puVar35[-0xe0] = 0x3f;
      *puVar39 = 5;
      *puVar39 = 0xf;
      *puVar39 = 5;
      *puVar8 = 0x8c01;
      *puVar8 = 0x8c00;
      puVar35 = DAT_a000246c;
      *DAT_a000246c = 0x2010;
      *puVar35 = 0;
      _DAT_1f202030 = 0;
      _DAT_1f2020f8 = 0;
      _DAT_1f2020a8 = 0xc000;
      *puVar39 = 5;
      *puVar39 = 0xf;
      *puVar39 = 5;
      *puVar35 = 2;
      puVar8[-0x1e] = 0;
      FUN_a0001cd4(12000);
      puVar8[-0x1e] = 8;
      puVar8[-0x1e] = 0xc;
      FUN_a0001cd4(12000);
      puVar8[-0x1e] = 0xe;
      FUN_a0001cd4(12000);
      puVar8[-0x1e] = 0xf;
      FUN_a0001cd4(12000);
      FUN_a0001cd4(12000);
      FUN_a0001cd4(12000);
      *puVar39 = 5;
      *puVar39 = 0xf;
      *puVar39 = 5;
      *puVar26 = 0x7ffe;
      goto LAB_a00028c0;
    }
    uart_puts(DAT_a0003240);
  }
  puVar13 = DAT_a0002bcc;
  puVar36 = DAT_a0002bc8;
  *DAT_a0002bc8 = *DAT_a0002bc8 & 0xfeff;
  if ((*puVar13 & 0x800) != 0) {
    uVar24 = (*puVar13 & 0x7ff) >> 5;
    uart_puts(DAT_a0002bd0);
    uart_put_hex16(uVar24);
    uart_puts(DAT_a0002bd4);
    *DAT_a0002bd8 = *DAT_a0002bd8 & 0x81ff | (ushort)(uVar24 << 9);
  }
  if ((int)((uint)*DAT_a0002bdc << 0x10) < 0) {
    uVar24 = (*DAT_a0002bdc & 0x7fff) >> 0xc;
    uart_puts(DAT_a0002be0);
    uart_put_hex16(uVar24);
    uart_puts(DAT_a0002bd4);
    uVar24 = uVar24 - 1 & 0xffff;
    if (uVar24 < 7) {
      iVar25 = (int)*(char *)(DAT_a0002be8 + uVar24);
    }
    else {
      iVar25 = 0;
    }
    FUN_a0001128(0xe,DAT_a0002be4,8,iVar25);
    FUN_a0001128(8,DAT_a0002bec,0,iVar25);
    FUN_a0001128(10,DAT_a0002bec,4,iVar25);
    FUN_a0001128(0xc,DAT_a0002bec,8,iVar25);
    FUN_a0001128(0xd,DAT_a0002bec,0xc,iVar25);
  }
  if ((int)((uint)*puVar13 << 0x10) < 0) {
    uVar24 = (*puVar13 & 0x7fff) >> 0xc;
    uart_puts(DAT_a0002bf0);
    uart_put_hex16(uVar24);
    uart_puts(DAT_a0002bd4);
    uVar24 = uVar24 - 1 & 0xffff;
    if (uVar24 < 7) {
      iVar25 = (int)*(char *)(DAT_a0002be8 + uVar24);
    }
    else {
      iVar25 = 0;
    }
    FUN_a0001128(6,DAT_a0002be4,0,iVar25);
    FUN_a0001128(0,DAT_a0002bf4,0,iVar25);
    FUN_a0001128(2,DAT_a0002bf4,4,iVar25);
    FUN_a0001128(4,DAT_a0002bf4,8,iVar25);
    FUN_a0001128(5,DAT_a0002bf4,0xc,iVar25);
  }
  FUN_a00015fc(0x2a0,DAT_a0002bf8);
  puVar46 = DAT_a0002c0c;
  puVar14 = DAT_a0002c00;
  puVar11 = DAT_a0002bfc;
  *DAT_a0002c00 = 0x15;
  puVar14[1] = 0x80;
  puVar14[4] = 8;
  puVar14[5] = 0x20;
  puVar14[8] = 0;
  puVar14[9] = 4;
  *puVar11 = 0xff;
  puVar11[1] = 0xff;
  puVar11[4] = 0x10;
  puVar11[5] = 0x32;
  puVar11[8] = 0x54;
  puVar11[9] = 0x76;
  puVar11[0xc] = 0x98;
  puVar11[0xd] = 0xba;
  puVar11[0x10] = 0xdc;
  *puVar46 = 0xfe;
  puVar46[0x17] = 0;
  puVar46[0x18] = 0;
  puVar46[0x1f] = 0x15;
  puVar46[0x20] = 0x80;
  puVar46[0x23] = 8;
  puVar46[0x24] = 0x20;
  puVar46[0x27] = 0;
  puVar46[0x28] = 4;
  puVar46[0x2f] = 0xff;
  puVar46[0x30] = 0xff;
  puVar46[0x33] = 0x10;
  puVar46[0x34] = 0x32;
  puVar46[0x37] = 0x54;
  puVar46[0x38] = 0x76;
  puVar46[0x3b] = 0x98;
  puVar46[0x3c] = 0xba;
  puVar46[0x3f] = 0xdc;
  puVar46[0x40] = 0xfe;
  puVar46[0x5f] = 0x15;
  puVar46[0x60] = 0x80;
  puVar46[99] = 8;
  puVar46[100] = 0x20;
  puVar46[0x67] = 0;
  puVar46[0x68] = 4;
  puVar46[0x6f] = 0xff;
  puVar46[0x70] = 0xff;
  puVar46[0x73] = 0x10;
  puVar46[0x74] = 0x32;
  puVar46[0x77] = 0x54;
  puVar46[0x78] = 0x76;
  puVar46[0x7b] = 0x98;
  puVar46[0x7c] = 0xba;
  puVar46[0x7f] = 0xdc;
  puVar46[0x80] = 0xfe;
  puVar46[0x9f] = 0x15;
  puVar11 = DAT_a0002c10;
  *DAT_a0002c10 = 0x80;
  puVar11[3] = 8;
  puVar11[4] = 0x20;
  puVar11 = DAT_a0002c14;
  *DAT_a0002c14 = 0;
  puVar11[1] = 4;
  puVar11 = DAT_a0002c18;
  *DAT_a0002c18 = 0xff;
  puVar11[1] = 0xff;
  puVar11[4] = 0x10;
  puVar11[5] = 0x32;
  puVar11[8] = 0x54;
  puVar11[9] = 0x76;
  puVar11[0xc] = 0x98;
  *DAT_a0002c1c = 0xba;
  *DAT_a0002c20 = 0xdc;
  puVar11 = DAT_a0002c04;
  *DAT_a0002c04 = 0xfe;
  puVar11[0x9b] = 0xe1;
  puVar11[0x9c] = 0x80;
  puVar11[-0x1a1] = 2;
  puVar11[-0x1a0] = 0;
  puVar11[-0x19d] = 0x1e;
  puVar11[-0x19c] = 0;
  puVar11[-0x191] = 0x18;
  puVar11[-400] = 0;
  puVar11[-0x18d] = 8;
  puVar11 = DAT_a0002c08;
  *DAT_a0002c08 = 0x40;
  puVar11[3] = 2;
  puVar11[4] = 2;
  puVar11[0x1b] = 0xe1;
  puVar11[0x1c] = 0xff;
  uart_puts(DAT_a0002f5c);
  uVar32 = DAT_a0002f64;
  if ((*DAT_a0002f60 & 0x18) != 0) {
    *DAT_a0002f60 = 0xc0;
    puVar14 = DAT_a00034b8;
    puVar8 = DAT_a0003474;
    puVar11 = DAT_a0003470;
    *DAT_a0003470 = 0x11;
    puVar11[0x1e4] = 0xb0;
    FUN_a0001cd4(0x4b0);
    puVar35 = DAT_a000347c;
    puVar11 = DAT_a0003478;
    *DAT_a0003478 = 0x10;
    puVar11[1] = 1;
    *(undefined1 *)puVar35 = 0x2f;
    puVar11[0x409] = 0xc;
    *puVar35 = 0x40f;
    puVar11 = DAT_a0003480;
    *puVar8 = 0x7f05;
    *puVar14 = 0;
    *puVar11 = 10;
    *puVar11 = 0x28;
    *(undefined2 *)(puVar11 + -0x3bc) = 0x2088;
    *(undefined2 *)(puVar11 + -0x3c0) = 0x8051;
    *(undefined2 *)(puVar11 + -0x3fc) = 0x2084;
    *puVar35 = 0x426;
    *puVar8 = 0x6bc3;
    *puVar14 = 0x3f;
    FUN_a0001cd4(0x4b0);
    *puVar8 = 0x69c3;
    *puVar14 = 0x3f;
    FUN_a0001cd4(0x4b0);
    *puVar8 = 1;
    uVar32 = DAT_a0003484;
    *puVar14 = 0;
    puVar46 = puVar14 + 0x800;
    *puVar8 = 0x7f03;
    puVar39 = puVar8 + 0x400;
    uart_puts(uVar32);
    puVar11 = DAT_a000348c;
    puVar35 = DAT_a0003488;
    *(undefined1 *)DAT_a0003488 = 0x2f;
    *puVar11 = 0xc;
    *puVar35 = 0x40f;
    *puVar39 = 0x7f05;
    *puVar46 = 0;
    puVar11[0x3ef] = 10;
    puVar11[0x3ef] = 0x28;
    *(undefined2 *)(puVar11 + 0x33) = 0x2088;
    *(undefined2 *)(puVar11 + 0x2f) = 0x8051;
    *(undefined2 *)(puVar11 + -0xd) = 0x2084;
    *puVar35 = 0x426;
    *puVar39 = 0x6bc3;
    *puVar46 = 0x3f;
    FUN_a0001cd4(0x4b0);
    *puVar39 = 0x69c3;
    *puVar46 = 0x3f;
    FUN_a0001cd4(0x4b0);
    *puVar39 = 1;
    uVar32 = DAT_a0003490;
    *puVar46 = 0;
    *puVar39 = 0x7f03;
    uart_puts(uVar32);
    puVar35 = DAT_a0003494;
    *(undefined1 *)DAT_a0003494 = 0x2f;
    *DAT_a0003498 = 0xc;
    *puVar35 = 0x40f;
    puVar11 = DAT_a000349c;
    puVar8[0x800] = 0x7f05;
    puVar14[0x1000] = 0;
    *puVar11 = 10;
    *puVar11 = 0x28;
    *(undefined2 *)(puVar11 + -0xfbc) = 0x2088;
    *(undefined2 *)(puVar11 + -0xfc0) = 0x8051;
    *(undefined2 *)(puVar11 + -0xffc) = 0x2084;
    *puVar35 = 0x426;
    puVar8[0x800] = 0x6bc3;
    puVar14[0x1000] = 0x3f;
    FUN_a0001cd4(0x4b0);
    puVar8[0x800] = 0x69c3;
    puVar14[0x1000] = 0x3f;
    FUN_a0001cd4(0x4b0);
    puVar8[0x800] = 1;
    uVar32 = DAT_a00034a0;
    puVar14[0x1000] = 0;
    puVar8[0x800] = 0x7f03;
    uart_puts(uVar32);
    uVar32 = DAT_a00034a4;
  }
  uart_puts(uVar32);
  puVar14 = DAT_a0002f6c;
  puVar11 = DAT_a0002f68;
  *DAT_a0002f6c = 0x88;
  puVar14[1] = 0;
  puVar14[-3] = 1;
  *puVar11 = 0x37;
  puVar11[-3] = 0x4b;
  puVar11[-4] = 199;
  *(undefined2 *)(puVar11 + -0x44) = 0x4bc7;
  *(undefined2 *)(puVar11 + -0x40) = 0x37;
  puVar46 = DAT_a0002ff8;
  local_7c = DAT_a0002f74;
  *DAT_a0002f70 = 1;
  puVar14[-3] = 0;
  FUN_a0001cd4(0x4b0);
  uart_puts(DAT_a0002f78);
  puVar11 = DAT_a0002f7c;
  *DAT_a0002f7c = 0x18;
  puVar11[0x24] = 4;
  *puVar44 = 0x30;
  puVar44 = DAT_a0002f80;
  *DAT_a0002f80 = 1;
  puVar44[0x214] = 0x84;
  *puVar7 = 0;
  *puVar46 = 4;
  *puVar46 = 0x15;
  puVar44 = DAT_a0002f84;
  *DAT_a0002f84 = 0x10;
  puVar44[-0xb8] = 0x10;
  puVar44[-0xb8] = puVar44[-0xb8] | 0x20;
  FUN_a0001d10();
  puVar44 = DAT_a0002f88;
  *(undefined1 *)local_7c = 1;
  uVar32 = DAT_a0002f8c;
  *puVar44 = 0x80;
  puVar44 = DAT_a0002f90;
  *DAT_a0002f90 = 3;
  puVar44[1] = 0;
  *puVar46 = 4;
  *puVar46 = 0x14;
  uart_puts(uVar32);
  uVar24 = FUN_a00011c8(2);
  uVar34 = FUN_a00011c8(3);
  uVar27 = FUN_a00011c8(0xd);
  uVar28 = FUN_a00011c8(0xe);
  uVar29 = FUN_a00011c8(0xf);
  uVar32 = DAT_a0002f98;
  if ((uVar24 & 0x10) != 0) {
    *DAT_a0002f94 = *DAT_a0002f94 & 0xfe1f | (ushort)((uVar24 & 0xf) << 5);
    uart_puts(uVar32);
    uart_put_hex16(uVar24 & 0xf);
    uart_puts(DAT_a0002f9c);
    uVar32 = DAT_a0002fa4;
    puVar13 = DAT_a0002fa0;
    uVar47 = uVar28 & 0xf;
    *DAT_a0002fa0 = *DAT_a0002fa0 & 0xfff8 | (ushort)(uVar47 >> 1);
    puVar13[-2] = puVar13[-2] & 0x7fff | (ushort)(uVar47 << 0xf);
    uart_puts(uVar32);
    uart_put_hex16(uVar47);
    uart_puts(DAT_a0002f9c);
  }
  uVar32 = DAT_a0002fac;
  if ((uVar28 & 0x1000) != 0) {
    uVar47 = (uVar28 & 0xff) >> 4;
    *DAT_a0002fa8 = *DAT_a0002fa8 & 0xfe1f | (ushort)(uVar47 << 5);
    uart_puts(uVar32);
    uart_put_hex16(uVar47);
    uart_puts(DAT_a0002f9c);
    uVar32 = DAT_a0002fb4;
    puVar13 = DAT_a0002fb0;
    uVar47 = (uVar28 & 0xfff) >> 8;
    *DAT_a0002fb0 = *DAT_a0002fb0 & 0xfff8 | (ushort)((uVar28 << 0x14) >> 0x1d);
    puVar13[-2] = puVar13[-2] & 0x7fff | (ushort)(uVar47 << 0xf);
    uart_puts(uVar32);
    uart_put_hex16(uVar47);
    uart_puts(DAT_a0002f9c);
  }
  uVar17 = DAT_a00034ac;
  uVar32 = DAT_a0002fbc;
  if ((int)(uVar27 << 0x12) < 0) {
    uVar47 = (uVar27 & 0x1ff) >> 5;
    *DAT_a0002fb8 = *DAT_a0002fb8 & 0xfe1f | (ushort)(uVar47 << 5);
    uart_puts(uVar32);
    uart_put_hex16(uVar47);
    uart_puts(DAT_a0002f9c);
    uVar47 = uVar27 << 0x13;
LAB_a0002e12:
    uVar32 = DAT_a0002fc4;
    puVar13 = DAT_a0002fc0;
    *DAT_a0002fc0 = *DAT_a0002fc0 & 0xfff8 | (ushort)(uVar47 >> 0x1d);
    puVar13[-2] = puVar13[-2] & 0x7fff | (ushort)((uVar47 >> 0x1c) << 0xf);
    uart_puts(uVar32);
    uart_put_hex16(uVar47 >> 0x1c);
    uart_puts(DAT_a0002f9c);
  }
  else if ((uVar28 & 0x1000) != 0) {
    uVar47 = (uVar28 & 0xff) >> 4;
    *DAT_a00034a8 = *DAT_a00034a8 & 0xfe1f | (ushort)(uVar47 << 5);
    uart_puts(uVar17);
    uart_put_hex16(uVar47);
    uart_puts(DAT_a00034b0);
    uVar47 = uVar28 << 0x14;
    goto LAB_a0002e12;
  }
  uVar32 = DAT_a0002fcc;
  if ((uVar24 & 0x4000000) != 0) {
    uVar47 = (uVar24 & 0xffff) >> 10;
    *DAT_a0002fc8 = *DAT_a0002fc8 & 0xff80 | (ushort)(uVar47 << 1) | 1;
    uart_puts(uVar32);
    uart_put_hex16(uVar47);
    uart_puts(DAT_a0002f9c);
    uart_puts(DAT_a0002fd0);
    uart_put_hex16((uVar24 & 0x3ffffff) >> 0x10);
    uart_puts(DAT_a0002f9c);
  }
  puVar16 = DAT_a0002ffc;
  uVar32 = DAT_a0002fdc;
  puVar15 = DAT_a0002fd8;
  puVar13 = DAT_a0002fd4;
  if ((uVar34 & 0x100000) != 0) {
    *DAT_a0002fd4 = *DAT_a0002fd4 | 4;
    *puVar15 = *puVar15 | 0x8000;
    *puVar16 = *puVar16 & 0xffe0 | (ushort)(uVar34 & 0x1f);
    uart_puts(uVar32);
    uart_put_hex16(uVar34 & 0x1f);
    uart_puts(DAT_a0002f9c);
    uVar32 = DAT_a0002fe0;
    uVar24 = (uVar34 & 0x3ff) >> 5;
    *puVar16 = *puVar16 & 0xe0ff | (ushort)(uVar24 << 8);
    uart_puts(uVar32);
    uart_put_hex16(uVar24);
    uart_puts(DAT_a0002f9c);
    uVar32 = DAT_a0002fe4;
    uVar24 = (uVar34 & 0x3fff) >> 10;
    *puVar13 = *puVar13 & 0xf87f | (ushort)(uVar24 << 7);
    uart_puts(uVar32);
    uart_put_hex16(uVar24);
    uart_puts(DAT_a0002f9c);
    uVar32 = DAT_a0002fec;
    *DAT_a0002fe8 = *DAT_a0002fe8 & 0xfff0 | (ushort)((uVar34 & 0xfffff) >> 0x10);
    uart_puts(uVar32);
    uart_put_hex16((uVar34 & 0xfffff) >> 0x10);
    uart_puts(DAT_a0002f9c);
    *DAT_a0002ff0 = *DAT_a0002ff0 | 1;
  }
  uVar32 = DAT_a00031f4;
  puVar13 = DAT_a0002ff4;
  iVar25 = uVar28 << 0x10;
  if (iVar25 < 0) {
    uVar37 = (ushort)uVar34 >> 0xe;
    *DAT_a0002ff4 = *DAT_a0002ff4 & 0xfffc | uVar37;
    puVar13[2] = puVar13[2] & 0xfffc | uVar37;
    puVar13[4] = puVar13[4] & 0xfffc | uVar37;
    puVar13[6] = puVar13[6] & 0xfffc | uVar37;
    puVar13[8] = uVar37 | puVar13[8] & 0xfffc;
    uart_puts(uVar32);
    uart_put_hex16((uVar34 & 0xffff) >> 0xe);
    uart_puts(DAT_a00031f8);
    uVar32 = DAT_a0003200;
    uVar24 = (uVar28 & 0x7fff) >> 0xd;
    *DAT_a00031fc = *DAT_a00031fc & 0xe7ff | (ushort)(uVar24 << 0xb);
    uart_puts(uVar32);
    uart_put_hex16(uVar24);
    iVar25 = uart_puts(DAT_a00031f8);
  }
  uVar32 = DAT_a0003208;
  if ((uVar29 & 0x80) != 0) {
    *DAT_a0003204 = *DAT_a0003204 & 0xff80 | (ushort)(uVar29 & 0x7f);
    uart_puts(uVar32);
    uart_put_hex16(uVar29 & 0x7f);
    iVar25 = uart_puts(DAT_a00031f8);
  }
  uVar32 = DAT_a0003210;
  if ((uVar29 & 0x8000) != 0) {
    *DAT_a000320c = *DAT_a000320c & 0xff80 | (ushort)((uVar29 & 0x7fff) >> 8);
    uart_puts(uVar32);
    uart_put_hex16((uVar29 & 0x7fff) >> 8);
    iVar25 = uart_puts(DAT_a00031f8);
  }
  uVar32 = DAT_a0003218;
  if ((uVar29 & 0x800000) != 0) {
    *DAT_a0003214 = *DAT_a0003214 & 0xff80 | (ushort)((uVar29 & 0x7fffff) >> 0x10);
    uart_puts(uVar32);
    uart_put_hex16((uVar29 & 0x7fffff) >> 0x10);
    iVar25 = uart_puts(DAT_a00031f8);
  }
  uVar32 = DAT_a0003224;
  puVar13 = DAT_a0003220;
  if ((int)(uVar28 << 5) < 0) {
    *DAT_a000321c = *DAT_a000321c & 0xffe0 | (ushort)((uVar28 & 0x1fffff) >> 0x10);
    uart_puts(uVar32);
    uart_put_hex16((uVar28 & 0x1fffff) >> 0x10);
    uart_puts(DAT_a00031f8);
    uVar32 = DAT_a0003228;
    *puVar13 = *puVar13 & 0xffe0 | (ushort)((uVar28 & 0x3ffffff) >> 0x15);
    uart_puts(uVar32);
    uart_put_hex16((uVar28 & 0x3ffffff) >> 0x15);
    uart_puts(DAT_a00031f8);
    uVar32 = DAT_a000322c;
    uVar24 = (uVar27 & 0x1fffff) >> 0x10;
    *puVar13 = *puVar13 & 0xe0ff | (ushort)((uVar27 & 0x1f) << 8);
    uart_puts(uVar32);
    puVar13 = DAT_a0003230;
    uart_put_hex16(uVar27 & 0x1f);
    uart_puts(DAT_a00031f8);
    uVar32 = DAT_a0003234;
    *puVar13 = *puVar13 & 0xffe0 | (ushort)((uVar27 & 0x1fff) >> 8);
    uart_puts(uVar32);
    uart_put_hex16((uVar27 & 0x1fff) >> 8);
    uart_puts(DAT_a00031f8);
    uVar32 = DAT_a0003238;
    *puVar13 = *puVar13 & 0xe0ff | (ushort)(uVar24 << 8);
    uart_puts(uVar32);
    uart_put_hex16(uVar24);
    iVar25 = uart_puts(DAT_a00031f8);
  }
  puVar13 = DAT_a000323c;
  *puVar36 = *puVar36 & 0xfeff;
  uVar51 = FUN_a000119c(iVar25,*puVar13 & 7);
  iVar25 = (int)((ulonglong)uVar51 >> 0x20);
  if ((int)uVar51 == 0) {
    if ((iVar25 == 2) || (iVar25 - 5U < 2)) {
      do {
                    /* WARNING: Do nothing block with infinite loop */
      } while( true );
    }
  }
  else if ((iVar25 == 3) || (iVar25 - 5U < 2)) {
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  *puVar36 = *puVar36 & 0xfeff;
  if ((*puVar13 & 0x7f) >> 5 == 1) {
    uart_puts(DAT_a00034b4);
    do {
                    /* WARNING: Do nothing block with infinite loop */
    } while( true );
  }
  *puVar36 = *puVar36 & 0xfeff;
  uVar24 = (*puVar13 & 0x7f) >> 5;
  if ((uVar24 != 0) && (uVar24 != 3)) goto LAB_a00034fe;
  uVar51 = FUN_a000119c();
  puVar44 = DAT_a0003bcc;
  uVar24 = (uint)((ulonglong)uVar51 >> 0x20) & 7;
  if ((int)uVar51 == 0) {
    if (uVar24 != 4) goto LAB_a00034fe;
    uVar37 = *DAT_a0003bd4 | 3;
    puVar36 = DAT_a0003bd4;
  }
  else {
    if (uVar24 == 1) {
      *DAT_a0003804 = 0;
      *DAT_a0003808 = *DAT_a0003808 | 8;
      goto LAB_a00034fe;
    }
    if (uVar24 == 2) {
      *DAT_a0003bc8 = 0xffff;
      goto LAB_a00034fe;
    }
    if (uVar24 != 4) goto LAB_a00034fe;
    *DAT_a0003bcc = 0;
    *(undefined2 *)(puVar44 + 0x7c454) = 0xffff;
    puVar36 = DAT_a0003bd0;
    *DAT_a0003bd0 = *DAT_a0003bd0 & 0xefff;
    puVar36[0x2a0] = puVar36[0x2a0] | 1;
    uVar37 = puVar36[0x118] | 0x100;
    puVar36 = puVar36 + 0x118;
  }
  *puVar36 = uVar37;
LAB_a00034fe:
  puVar8 = DAT_a000380c;
  puVar35 = DAT_a000380c + -0x120;
  *DAT_a000380c = 0;
  puVar8[0x20] = 0;
  puVar8[0x40] = 0;
  puVar8[0x60] = 0;
  *puVar35 = 0;
  puVar8[-0x60] = 0;
  *DAT_a0003810 = 0x8c08;
  _DAT_20000000 = 0x11111111;
  _DAT_22000000 = 0x33333333;
  _DAT_24000000 = 0x22222222;
  _DAT_28000000 = 0x44444444;
  _DAT_30000000 = 0x88888888;
  uart_puts(DAT_a0003814);
  *DAT_a0003818 = (ushort)(((uint)*DAT_a0003818 << 0x14) >> 0x14) | 0x9000;
  uVar32 = DAT_a0003828;
  puVar15 = DAT_a0003824;
  puVar13 = DAT_a0003820;
  puVar36 = DAT_a000381c;
  *DAT_a000381c = *DAT_a000381c | 1;
  *puVar36 = *puVar36 & 0xfffe;
  puVar8 = DAT_a000382c;
  *puVar13 = 0;
  *puVar8 = 0;
  puVar8[2] = 0xffff;
  puVar8[4] = 0x1fe;
  *DAT_a0003830 = 0x5aa5;
  uart_puts(uVar32);
  *puVar15 = 0;
  uart_put_hex16(1);
  *puVar13 = 1;
  do {
    uVar37 = *puVar13;
  } while (-1 < (int)((uint)uVar37 << 0x10));
  local_84 = 0;
  uVar32 = DAT_a0003bf0;
  if ((uVar37 & 0x6000) != 0) goto LAB_a0003b2a;
  uart_puts(DAT_a0003834);
  *puVar15 = uVar37 & 0x6000;
  *puVar13 = uVar37 & 0x6000;
  FUN_a000014c();
  uart_puts(DAT_a0003838);
  FUN_a00015fc(499,DAT_a000383c);
  uVar24 = (uint)*DAT_a0003840;
  if (*DAT_a0003844 == -6) goto LAB_a0003b32;
  uVar37 = *DAT_a0003840 & 1;
LAB_a0003616:
  pcVar42 = DAT_a0004114;
  if ((uVar24 & 0x24) == 0x20) {
    uart_puts(DAT_a0003848);
    puVar15 = DAT_a000388c;
    psVar20 = DAT_a0003888;
    pcVar42 = DAT_a0003884;
    puVar13 = DAT_a0003858;
    puVar36 = DAT_a0003854;
    if (*(int *)(*DAT_a000384c + 0x14000104) == DAT_a000385c) {
      iVar25 = *DAT_a000384c + 0x100;
    }
    else {
      iVar25 = 0x10000;
    }
    uart_puts(DAT_a0003850);
    uart_put_hex32(iVar25);
    uart_puts(DAT_a0003860);
    puVar16 = DAT_a0003864;
    local_84 = iVar25 + 0x14000000;
    uVar43 = *(ushort *)(&DAT_14000008 + iVar25);
    *pcVar42 = '\0';
    puVar19 = DAT_a0003868;
    *(undefined8 *)psVar20 = uVar50;
    *(undefined8 *)(psVar20 + 2) = uVar50;
    *(undefined1 *)local_7c = 1;
    *puVar16 = *puVar16 & 0xff00 | 0x9f;
    *puVar36 = *puVar36 & 0xfff0 | 1;
    *puVar36 = *puVar36 & 0xff0f;
    *puVar36 = *puVar36 & 0xf0ff;
    *puVar13 = *puVar13 & 0xfff0 | 3;
    *puVar13 = *puVar13 & 0xff0f;
    *puVar13 = *puVar13 & 0xf0ff;
    *puVar19 = *puVar19 | 1;
    *puVar19 = *puVar19 | 2;
    *puVar19 = *puVar19 | 4;
    *puVar19 = (ushort)(((uint)*puVar19 << 0x11) >> 0x11);
    *puVar19 = *puVar19 & 0xbfff;
    *puVar19 = *puVar19 & 0xdfff;
    *puVar15 = *puVar15 | 1;
    uVar24 = FUN_a0001a1c();
    if ((uVar24 & 1) == 0) {
      uart_puts(DAT_a000386c);
    }
    puVar16 = DAT_a0003870;
    if ((uVar24 & 1) == 0) {
LAB_a000399a:
      if (*pcVar42 != '\0') goto LAB_a0003b66;
      uart_puts(DAT_a0003ba0);
    }
    else {
      cVar5 = (char)*DAT_a0003870;
      cVar6 = (char)*DAT_a0003874;
      if (((cVar5 != -0x38) || ((int)(uint)*DAT_a0003870 >> 8 != 0x40)) || (cVar6 != '\x18')) {
        uVar33 = *DAT_a0003870 >> 8;
        iVar49 = 0;
        for (iVar25 = DAT_a0003878; *(char *)(iVar25 + 2) != '\0'; iVar25 = iVar25 + 0xc) {
          if (((*(char *)(iVar25 + 2) == cVar5) && (*(byte *)(iVar25 + 3) == uVar33)) &&
             (*(char *)(iVar25 + 4) == cVar6)) {
            puVar48 = (undefined4 *)(iVar49 * 0xc + DAT_a0003878);
            *(undefined4 *)psVar20 = *puVar48;
            *(undefined4 *)(psVar20 + 2) = puVar48[1];
            *(undefined4 *)(psVar20 + 4) = puVar48[2];
            uart_puts(DAT_a000387c);
            uart_put_hex8(cVar5);
            uart_put_hex8(uVar33);
            uart_put_hex8(cVar6);
            uart_puts(DAT_a0003880);
            puVar18 = DAT_a0003864;
            if ((*psVar20 == 0x506) || (*psVar20 == 0x508)) {
              *(undefined1 *)local_7c = 1;
              *puVar18 = *puVar18 & 0xff00 | 0x90;
              *puVar18 = *puVar18 & 0xff;
              puVar18 = DAT_a0003b90;
              *DAT_a0003b90 = *DAT_a0003b90 & 0xff00;
              *puVar18 = *puVar18 & 0xff;
              *puVar36 = *puVar36 & 0xfff0 | 4;
              *puVar36 = *puVar36 & 0xff0f;
              *puVar36 = *puVar36 & 0xf0ff;
              *puVar13 = *puVar13 & 0xfff0 | 2;
              *puVar13 = *puVar13 & 0xff0f;
              *puVar13 = *puVar13 & 0xf0ff;
              *puVar19 = *puVar19 | 1;
              *puVar19 = *puVar19 | 2;
              *puVar19 = *puVar19 | 4;
              *puVar19 = (ushort)(((uint)*puVar19 << 0x11) >> 0x11);
              *puVar19 = *puVar19 & 0xbfff;
              *puVar19 = *puVar19 & 0xdfff;
              *puVar15 = *puVar15 | 1;
              iVar25 = FUN_a0001a1c();
              if (iVar25 == 0) {
                uart_puts(DAT_a0003b94);
              }
              uVar33 = *puVar16;
              uVar3 = *puVar16;
              uart_puts(DAT_a0003b98);
              cVar5 = (char)uVar33;
              uVar24 = (uint)(uVar3 >> 8);
              uart_put_hex8(cVar5);
              uart_put_hex8(uVar24);
              uart_puts(DAT_a0003b9c);
              if (cVar5 == -0x3e) {
                if (uVar24 - 0x16 < 2) {
                  psVar20[3] = 6;
                  psVar20[4] = 4;
                }
              }
            }
            *pcVar42 = '\x01';
            break;
          }
          iVar49 = iVar49 + 1;
        }
        goto LAB_a000399a;
      }
      *(undefined1 *)(psVar20 + 2) = 0x18;
      *psVar20 = 0xb05;
      psVar20[1] = 0x40c8;
      psVar20[3] = 3;
      psVar20[5] = 0x101;
      *pcVar42 = '\x01';
LAB_a0003b66:
      cVar5 = (char)psVar20[1];
      if (cVar5 == 'h') {
        if (*pcVar42 != '\0') {
          FUN_a0001bcc(0);
          uart_puts(DAT_a0003bf4);
        }
        uVar22 = 0;
      }
      else {
        if (*pcVar42 != '\0') {
          if ((cVar5 == -0x38) || (cVar5 == -0x11)) {
            FUN_a0001b24(0x35,local_64);
            local_64[0] = local_64[0] | 2;
            FUN_a0001a58();
            uVar32 = DAT_a0003db0;
          }
          else if (cVar5 == '\v') {
            FUN_a0001b24(0x35,local_64);
            bVar1 = local_64[0];
            puVar16 = DAT_a0003db4;
            local_64[0] = local_64[0] | 2;
            *(undefined1 *)local_7c = 1;
            *puVar16 = *puVar16 & 0xff00 | 6;
            *puVar16 = *puVar16 & 0xff | 0x100;
            puVar16[2] = puVar16[2] & 0xff00 | 0x40;
            puVar18 = DAT_a0003db8;
            puVar16[2] = CONCAT11(bVar1,(char)puVar16[2]) | 0x200;
            *puVar18 = *puVar18 & 0xff00 | 5;
            *puVar36 = *puVar36 & 0xfff0 | 1;
            *puVar36 = *puVar36 & 0xff0f | 0x30;
            *puVar36 = *puVar36 & 0xf0ff | 0x100;
            *puVar13 = *puVar13 & 0xfff0;
            *puVar13 = *puVar13 & 0xff0f;
            *puVar13 = *puVar13 & 0xf0ff | 0x100;
            *puVar19 = *puVar19 | 1;
            *puVar19 = *puVar19 | 2;
            *puVar19 = *puVar19 | 4;
            *puVar19 = *puVar19 | 0x8000;
            *puVar19 = *puVar19 | 0x4000;
            *puVar19 = *puVar19 & 0xe7ff | 0x1000;
            *puVar19 = *puVar19 | 0x2000;
            *puVar15 = *puVar15 | 1;
            iVar25 = FUN_a0001a1c();
            uVar32 = DAT_a0003dc0;
            if (-1 < iVar25 << 0x1f) {
              uart_puts(DAT_a0003dbc);
              uVar32 = DAT_a0003dc0;
            }
          }
          else if (cVar5 == ' ') {
            if (*(char *)((int)psVar20 + 3) == 'p') {
              FUN_a0001b24(0x3a,local_64);
              FUN_a0001bcc(0x40);
              FUN_a0001b24(4,local_64);
              uVar32 = DAT_a0003dc4;
            }
            else {
              FUN_a0001bcc(0x40);
              uVar32 = DAT_a0003dc8;
            }
          }
          else if (cVar5 == '^') {
            FUN_a0001b24(0x35,local_64);
            local_64[0] = local_64[0] | 2;
            FUN_a0001a58();
            uVar32 = DAT_a0003dcc;
          }
          else {
            FUN_a0001bcc(0x40);
            uVar32 = DAT_a0003dd0;
          }
          uart_puts(uVar32);
        }
        uVar22 = 10;
      }
      *(undefined1 *)local_7c = uVar22;
    }
    puVar36 = DAT_a0003dd8;
    uVar24 = uVar43 + 0x20f & 0xfffffff0;
    iVar25 = coprocessor_movefromRt(0xf,0,in_cr14);
    coprocessor_movefromRt2(0xf,0,in_cr14);
    uVar32 = DAT_a0003ba4;
    if ((local_84 & 0xf) == 0) {
      iVar49 = 5000;
      do {
        uVar43 = *puVar36;
        uVar33 = uVar43 & 2;
        if ((uVar43 & 2) == 0) {
          *puVar36 = 0x1c;
          puVar8 = DAT_a00046b4;
          *DAT_a00046b4 = 0x4035;
          puVar8[2] = uVar33;
          puVar8[4] = (undefined2)local_84;
          *DAT_a00046b8 = (short)(local_84 >> 0x10);
          puVar36 = DAT_a00046bc;
          *DAT_a00046bc = uVar33;
          puVar36[2] = 0x3c0;
          puVar8 = DAT_a00046c0;
          *DAT_a00046c0 = (short)uVar24;
          puVar8[2] = (short)(uVar24 >> 0x10);
          *DAT_a00046c4 = 1;
          goto LAB_a00039c8;
        }
        FUN_a0001cd4(12000);
        iVar49 = iVar49 + -1;
        uVar32 = DAT_a0003dd4;
      } while (iVar49 != 0);
    }
    uart_puts(uVar32);
LAB_a00039c8:
    do {
    } while (-1 < (int)((uint)*DAT_a0003ba8 << 0x1c));
    *DAT_a0003ba8 = *DAT_a0003ba8 | 8;
    iVar49 = coprocessor_movefromRt(0xf,0,in_cr14);
    coprocessor_movefromRt2(0xf,0,in_cr14);
    uVar4 = (ulonglong)DAT_a0003bac;
    uart_puts(DAT_a0003bb0);
    uVar34 = (uint)((uint)(iVar49 - iVar25) * uVar4 >> 0x22);
    FUN_a0001824(uVar34);
    uart_puts(DAT_a0003bb4);
    FUN_a00046cc(uVar24 * 1000,uVar34);
    FUN_a0001824();
    uart_puts(DAT_a0003bb8);
LAB_a0003a0c:
    FUN_a000036c();
    FUN_a000165c();
    puVar13 = DAT_a000469c;
    puVar36 = DAT_a0004660;
    if ((*DAT_a0003bbc == DAT_a0003bc0) ||
       (uVar32 = DAT_a0003bc4, *DAT_a0003bbc == (int)DAT_a0003bbc + 0x2a8c5045)) {
      if (uVar37 == 0) {
LAB_a0004590:
        FUN_a00015fc(0x206,DAT_a0004694);
        FUN_a00015fc(0x1d7,DAT_a0004698);
        uVar37 = *DAT_a000469c;
        uVar43 = DAT_a000469c[2];
        uVar34 = 0;
        for (uVar24 = 0; uVar24 < uVar37 - 0x10; uVar24 = uVar24 + 4) {
          uVar34 = uVar34 + *(int *)(&DAT_23c00010 + uVar24);
        }
        if ((uint)uVar43 == (uVar34 & 0xffff)) {
          uart_puts(DAT_a00046a0);
          FUN_a00015fc(0x1ea,DAT_a00046a4);
          FUN_a00015fc(0x209,DAT_a00046a8);
          (*(code *)&SUB_23c00000)();
          return;
        }
        uart_puts(DAT_a00046ac);
        uart_put_hex16((uint)uVar43);
        uart_puts(DAT_a00046b0);
        uVar24 = 0;
        for (uVar34 = 0; uVar34 < uVar37 - 0x10; uVar34 = uVar34 + 4) {
          uVar24 = uVar24 + *(int *)(&DAT_23c00010 + uVar34);
        }
        uart_put_hex16(uVar24 & 0xffff);
        uVar32 = DAT_a00046b0;
      }
      else {
        uVar32 = DAT_a00044a0;
        if (*DAT_a0003bbc == DAT_a000449c) {
          puVar44 = &SUB_23c00000 +
                    (((uint)*DAT_a000469c - (uint)*DAT_a000465c) - (uint)*DAT_a0004660);
          uVar24 = (uint)*DAT_a000469c;
          uart_puts(DAT_a0004664);
          uart_puts(DAT_a0004668);
          uart_put_hex32(puVar44);
          uart_puts(DAT_a000466c);
          uart_puts(DAT_a0004670);
          uart_put_hex16(*puVar36);
          uart_puts(DAT_a000466c);
          FUN_a000036c();
          FUN_a000165c();
          uVar37 = *puVar13;
          uart_puts(DAT_a0004674);
          uart_put_hex32(uVar37 + 0x23c00100);
          uart_puts(DAT_a000466c);
          iVar25 = FUN_a00010c2(puVar44,0x100,uVar37 + 0x23c00100,0);
          uVar32 = DAT_a0004678;
          if (iVar25 != 0) {
            FUN_a000036c();
            FUN_a000165c();
            uart_puts(DAT_a000467c);
            uart_put_hex32(&SUB_23c00000);
            uart_puts(DAT_a000466c);
            uart_puts(DAT_a0004680);
            uart_put_hex32(uVar24);
            uart_puts(DAT_a000466c);
            uart_puts(DAT_a0004684);
            uart_put_hex32(&SUB_23c00000 + uVar24);
            uart_puts(DAT_a000466c);
            iVar25 = FUN_a00010c2(&SUB_23c00000,uVar24,&SUB_23c00000 + uVar24,puVar44);
            uVar32 = DAT_a0004688;
            if (iVar25 != 0) {
              uart_puts(DAT_a000468c);
              *DAT_a0004690 = *DAT_a0004690 | 4;
              goto LAB_a0004590;
            }
          }
        }
      }
    }
  }
  else {
    if ((uVar24 & 0x24) != 4) goto LAB_a0003a0c;
    uart_puts(DAT_a0004118);
    piVar21 = DAT_a0004178;
    pbVar38 = DAT_a000411c;
    if (*pcVar42 == '\0') {
LAB_a0003f5a:
      pcVar42 = (char *)*piVar21;
      if ((*(int *)(pbVar38 + 4) == 0x21aa) && (*pbVar38 == 0xef)) {
        local_68 = 0;
        local_66 = 0;
        iVar49 = 2;
        local_64[0] = 0;
        local_64[1] = 0;
        FUN_a0001700(&local_66,local_64,&local_68);
        uVar17 = DAT_a0004180;
        iVar25 = DAT_a000417c;
        uVar32 = DAT_a000414c;
        bVar1 = (byte)local_66;
        do {
          FUN_a0001574(1);
          FUN_a00015ac();
          uVar43 = *(ushort *)(iVar25 + 0x14);
          if (pcVar42 != (char *)0x0) {
            *pcVar42 = '\x01';
          }
          local_67 = 0;
          if ((pcVar42 != (char *)0x0) && (*pcVar42 != '\0')) {
            iVar30 = (uint)uVar43 * (uint)bVar1;
            *pcVar42 = '\0';
            uVar24 = FUN_a0001870(iVar30);
            if (uVar24 == 0) {
              uVar24 = FUN_a00018ec(&local_67);
              if (uVar24 == 0) goto LAB_a0003fbe;
              uart_puts(DAT_a0004170);
              uart_put_hex32(iVar30);
              uart_puts(DAT_a0004160);
              uart_put_hex8(uVar24 & 0xff);
              uart_puts(DAT_a000414c);
              FUN_a0001824(0x11d);
              uart_puts(DAT_a000414c);
              uVar32 = DAT_a0004174;
            }
            else {
              uart_puts(DAT_a000415c);
              uart_put_hex32(iVar30);
              uart_puts(DAT_a0004160);
              uart_put_hex8(uVar24 & 0xff);
              uart_puts(DAT_a000414c);
              FUN_a0001824(0x10f);
              uart_puts(DAT_a000414c);
              uVar32 = DAT_a0004164;
            }
            uart_puts(uVar32);
            uart_puts(DAT_a0004168);
            FUN_a0001824(0x2a1);
            uart_puts(DAT_a000414c);
            uVar31 = DAT_a000416c;
            goto LAB_a00040c4;
          }
LAB_a0003fbe:
          FUN_a000128c(0,0xf000,&SUB_23c00000,1);
          iVar30 = FUN_a00015ac();
          if (iVar30 == 0) {
            FUN_a0001574();
            goto LAB_a0003a0c;
          }
          uVar31 = DAT_a000444c;
          if (CONCAT11(local_64[1],local_64[0]) == 0) goto LAB_a00040c4;
          uart_puts(uVar17);
          uart_put_hex8(local_64[0]);
          uart_puts(uVar32);
          if (iVar49 == 1) goto code_r0xa0004000;
          iVar49 = 1;
          bVar1 = local_64[0];
        } while( true );
      }
      local_66 = 0;
      uVar24 = 0;
      local_64[0] = 0;
      local_64[1] = 0;
      local_68 = 0;
      uVar43 = *(ushort *)(DAT_a0004450 + 0x14);
      uVar51 = FUN_a00015e8(*(undefined2 *)(DAT_a0004450 + 0x18));
      iVar25 = DAT_a0004450;
      iVar49 = (int)((ulonglong)uVar51 >> 0x20);
      uVar28 = (uint)*(ushort *)(iVar49 + 0x12);
      *(char *)(iVar49 + 0x32) = (char)uVar51;
      uVar34 = (int)uVar28 >> ((uint)uVar51 & 0xff);
      *(short *)(iVar49 + 0x34) = (short)uVar34;
      uVar22 = FUN_a00015e8(uVar34 & 0xffff);
      *(undefined1 *)(extraout_r1 + 0x33) = uVar22;
      uVar23 = FUN_a00015e8((uint)uVar43);
      *(undefined2 *)(extraout_r1_00 + 0x36) = uVar23;
      uVar51 = FUN_a00015e8(uVar28);
      uVar34 = uVar43 - 1;
      *(short *)((int)((ulonglong)uVar51 >> 0x20) + 0x38) = (short)uVar51;
      uVar27 = 0xf000U >> ((uint)uVar51 & 0xff) & 0xffff;
      if ((uVar28 - 1 & 0xf000) != 0) {
        uVar27 = uVar27 + 1 & 0xffff;
      }
      FUN_a0001700(&local_66,local_64,&local_68);
      uVar43 = local_66;
LAB_a00041fa:
      uVar29 = (uint)uVar43;
      local_84 = (uint)local_68;
      for (uVar28 = 0; uVar28 < uVar27; uVar28 = uVar28 + 1) {
LAB_a000435e:
        if (pcVar42 != (char *)0x0) {
          uVar24 = uVar34 & uVar28;
          if (uVar24 == 0) {
            *pcVar42 = '\x01';
          }
          if (uVar34 == uVar24) {
            pcVar42[1] = '\x01';
          }
          if (uVar27 <= uVar28 + 1) {
            pcVar42[2] = '\x01';
          }
        }
        local_7c = (undefined2 *)(uint)*(ushort *)(iVar25 + 0x38);
        uVar47 = (uVar29 << (*(ushort *)(iVar25 + 0x36) & 0xff)) + uVar28;
        if ((pcVar42 == (char *)0x0) || (pcVar42[3] == '\0')) {
          iVar49 = FUN_a0001398(uVar47);
          if (iVar49 == 0) {
            uart_puts(DAT_a0004468);
            FUN_a0001824(0x185);
            uart_puts(DAT_a000446c);
            uart_puts(DAT_a0004470);
          }
          uVar24 = FUN_a00015ac();
          if (uVar24 != 0) {
            uart_puts(DAT_a0004474);
            uVar32 = 0x18e;
            goto LAB_a00043e4;
          }
LAB_a0004240:
          FUN_a000036c();
          if (*(char *)(iVar25 + 0x1a) == '\0') {
            uVar43 = 0;
          }
          else {
            uVar24 = FUN_a00015e8(*(undefined2 *)(iVar25 + 0x14));
            uVar43 = 0;
            if ((uVar47 >> (uVar24 & 0xff) & 1) != 0) {
              uVar43 = 0x1000;
              *DAT_a000445c = 0x1800;
              *DAT_a0004460 = 0x17;
            }
          }
          puVar44 = &SUB_23c00000 + (uVar28 << ((uint)local_7c & 0xff));
          if ((code *)((uint)puVar44 & 0xf0000000) == thunk_FUN_a0000010) {
            if (((uint)puVar44 & 0xfffffff) < 0xd0000) {
              uVar32 = 0;
            }
            else {
              uVar32 = 1;
            }
          }
          else {
            uVar32 = 1;
          }
          FUN_a000128c(uVar43,*(undefined2 *)(iVar25 + 0x12),puVar44,uVar32);
          FUN_a000128c(*(ushort *)(iVar25 + 0x12) | uVar43,*(undefined2 *)(iVar25 + 0x10),
                       DAT_a0004464,0);
          if (pcVar42 == (char *)0x0) {
            uVar24 = 0;
          }
          else if ((pcVar42[3] == '\0') || (uVar24 = 0, pcVar42[2] == '\0')) {
            uVar24 = 0;
          }
          else {
            FUN_a0001440();
            pcVar42[2] = '\0';
          }
        }
        else {
          local_67 = 0;
          if (*pcVar42 == '\0') {
LAB_a0004216:
            uVar24 = FUN_a00018ec(&local_67);
            uVar32 = DAT_a0004458;
            if (uVar24 != 0) goto LAB_a00043bc;
            if (pcVar42[1] == '\0') {
              uVar24 = FUN_a000199c(0x31);
            }
            else {
              uVar24 = FUN_a000199c(0x3f);
              pcVar42[1] = '\0';
            }
            if (uVar24 == 0) goto LAB_a0004240;
          }
          else {
            *pcVar42 = '\0';
            uVar24 = FUN_a0001870(uVar47);
            uVar32 = DAT_a000447c;
            if (uVar24 == 0) goto LAB_a0004216;
LAB_a00043bc:
            uart_puts(uVar32);
            uart_put_hex32(uVar47);
            uart_puts(DAT_a0004480);
            uart_put_hex8(uVar24 & 0xff);
            uart_puts(DAT_a000446c);
          }
          uart_puts(DAT_a0004484);
          uVar32 = 0x178;
LAB_a00043e4:
          FUN_a0001824(uVar32);
          uart_puts(DAT_a000446c);
          uart_puts(DAT_a0004488);
        }
        if (*DAT_a0004464 != -1) {
          uart_puts(DAT_a000448c);
LAB_a0004402:
          uart_puts(DAT_a0004490);
          uart_put_hex8(uVar29 + 1 & 0xff);
          uart_puts(DAT_a000446c);
          if (local_84 == 0) {
            uVar31 = DAT_a0004498;
            if (CONCAT11(local_64[1],local_64[0]) != 0) {
              uVar24 = 3;
              uart_puts(DAT_a0004494);
              goto LAB_a0004326;
            }
            goto LAB_a00040c4;
          }
          uVar29 = uVar29 + 1 & 0xffff;
          local_84 = local_84 - 1 & 0xff;
          goto LAB_a000435e;
        }
        if (uVar24 == 0xb) {
          uVar31 = DAT_a0004498;
          if (CONCAT11(local_64[1],local_64[0]) == 0) goto LAB_a00040c4;
LAB_a0004326:
          uart_puts(DAT_a0004478);
          uart_put_hex8(local_64[0]);
          uart_puts(DAT_a000446c);
          uVar43 = CONCAT11(local_64[1],local_64[0]);
          local_64[0] = 0;
          local_64[1] = 0;
          goto LAB_a00041fa;
        }
        if (uVar24 == 3) goto LAB_a0004402;
      }
      uVar32 = DAT_a0004454;
      if (uVar24 == 0) goto LAB_a0003a0c;
    }
    else {
      *DAT_a0004120 = 0x20;
      puVar8 = DAT_a0004124;
      *DAT_a0004124 = 0;
      puVar8[-0x1006ac] = 0;
      puVar8[-0x101036] = 8;
      iVar25 = FUN_a0001440();
      uVar32 = DAT_a0004158;
      if (iVar25 != 0) {
        local_64[0] = 0;
        local_64[1] = 0;
        uStack_62 = 0;
        *piVar21 = DAT_a0004128;
        puVar8 = DAT_a000412c;
        *DAT_a000412c = 7;
        puVar8[0x12] = 0;
        puVar8[0x14] = 0;
        puVar8[-0x18] = 0x9f;
        puVar8[-4] = 2;
        puVar8[-2] = 3;
        puVar8[2] = 1;
        iVar25 = FUN_a000137c();
        if (iVar25 != 0) {
          FUN_a000134c(local_64);
          *DAT_a0004130 = 1;
        }
        uVar24 = CONCAT22(uStack_62,CONCAT11(local_64[1],local_64[0]));
        *(uint *)(pbVar38 + 4) = (uVar24 & 0xffffff) >> 8;
        *pbVar38 = local_64[0];
        if (((uVar24 & 0xfffff7ff) == 0xd1c8) || (uVar24 == DAT_a0004134)) {
          local_68 = 0;
          FUN_a00014f0(&local_68,0xb0);
          iVar25 = DAT_a0004138;
          uVar24 = CONCAT22(uStack_62,CONCAT11(local_64[1],local_64[0]));
          *piVar21 = DAT_a0004138;
          uVar32 = DAT_a000413c;
          if ((uVar24 & 0xfffff7ff) == 0xd1c8) {
            local_67 = 0;
            iVar49 = FUN_a00014f0(&local_67,0xd0);
            if ((iVar49 == 0) && ((local_67 & 0x60) != 0x20)) {
              local_66 = CONCAT11(local_66._1_1_,local_67) & 0xff9f | 0x20;
              FUN_a00014a8(&local_66,0xd0);
            }
            if (-1 < (int)((uint)local_68 << 0x1f)) {
              local_68 = local_68 | 1;
              FUN_a00014a8(&local_68,0xb0);
            }
            *(bool *)(iVar25 + 3) = CONCAT22(uStack_62,CONCAT11(local_64[1],local_64[0])) == 0xd9c8;
            *(undefined4 *)(iVar25 + 0xc) = DAT_a000413c;
            *(undefined1 *)(iVar25 + 4) = 1;
          }
          else if (uVar24 == DAT_a0004134) {
            *(undefined1 *)(iVar25 + 3) = 1;
            *(undefined4 *)(iVar25 + 0xc) = uVar32;
            *(undefined1 *)(iVar25 + 4) = 7;
            FUN_a0001574();
          }
          else {
            *(undefined1 *)(iVar25 + 3) = 0;
          }
        }
        else if (local_64[0] == 0xc2) {
          *piVar21 = DAT_a0004150;
          FUN_a000153c(1);
        }
        else {
          FUN_a000153c(1);
          uart_puts(DAT_a0004154);
        }
        iVar25 = DAT_a000417c;
        puVar8 = DAT_a0004140;
        *DAT_a0004140 = 0x3800;
        puVar8[-0x1a] = 0x17;
        *local_7c = 0;
        do {
          FUN_a0001398(local_84 * *(ushort *)(iVar25 + 0x14) + 1);
          iVar49 = FUN_a00015ac();
          if (iVar49 == 0) {
            FUN_a000128c(0,0x200,DAT_a0004144,0);
            *pcVar42 = '\0';
            goto LAB_a0003f5a;
          }
          local_84 = local_84 + 2;
        } while (local_84 != 10);
        *pcVar42 = '\0';
        uVar32 = DAT_a0004158;
      }
    }
  }
  goto LAB_a0003b2a;
code_r0xa0004000:
  uart_puts(DAT_a0004148);
  uart_put_hex8(iVar30);
  uVar31 = DAT_a000414c;
LAB_a00040c4:
  uart_puts(uVar31);
  uVar32 = DAT_a0004454;
LAB_a0003b2a:
  uart_puts(uVar32);
  FUN_a00017f8();
  uVar24 = extraout_r3;
LAB_a0003b32:
  uVar37 = 1;
  goto LAB_a0003616;
}



/* FUN_a00046cc @ a00046cc */

ulonglong FUN_a00046cc(uint param_1,uint param_2)

{
  uint uVar1;
  int iVar2;
  bool bVar3;
  bool bVar4;
  bool bVar5;
  bool bVar6;
  bool bVar7;
  bool bVar8;
  bool bVar9;
  bool bVar10;
  bool bVar11;
  bool bVar12;
  bool bVar13;
  bool bVar14;
  bool bVar15;
  bool bVar16;
  bool bVar17;
  bool bVar18;
  bool bVar19;
  bool bVar20;
  bool bVar21;
  bool bVar22;
  bool bVar23;
  bool bVar24;
  bool bVar25;
  bool bVar26;
  bool bVar27;
  bool bVar28;
  bool bVar29;
  bool bVar30;
  bool bVar31;
  bool bVar32;
  bool bVar33;
  ulonglong uVar34;
  
  if (param_2 - 1 == 0) {
    return CONCAT44(param_2,param_1);
  }
  if (param_2 == 0) {
    uVar1 = func_0xa00055b4(8);
    return (ulonglong)uVar1;
  }
  if (param_1 <= param_2) {
    return CONCAT44(param_2,(uint)(param_1 == param_2));
  }
  if ((param_2 & param_2 - 1) == 0) {
    return CONCAT44(param_2,param_1 >> (0x1fU - LZCOUNT(param_2) & 0xff));
  }
  iVar2 = 0x1f - (LZCOUNT(param_2) - LZCOUNT(param_1));
  if (iVar2 == 0) {
    bVar3 = param_2 << 0x1f <= param_1;
    if (bVar3) {
      param_1 = param_1 + param_2 * -0x80000000;
    }
    bVar4 = param_2 << 0x1e <= param_1;
    if (bVar4) {
      param_1 = param_1 + param_2 * -0x40000000;
    }
    bVar5 = param_2 << 0x1d <= param_1;
    if (bVar5) {
      param_1 = param_1 + param_2 * -0x20000000;
    }
    bVar6 = param_2 << 0x1c <= param_1;
    if (bVar6) {
      param_1 = param_1 + param_2 * -0x10000000;
    }
    bVar7 = param_2 << 0x1b <= param_1;
    if (bVar7) {
      param_1 = param_1 + param_2 * -0x8000000;
    }
    bVar8 = param_2 << 0x1a <= param_1;
    if (bVar8) {
      param_1 = param_1 + param_2 * -0x4000000;
    }
    bVar9 = param_2 << 0x19 <= param_1;
    if (bVar9) {
      param_1 = param_1 + param_2 * -0x2000000;
    }
    bVar10 = param_2 << 0x18 <= param_1;
    if (bVar10) {
      param_1 = param_1 + param_2 * -0x1000000;
    }
    bVar11 = param_2 << 0x17 <= param_1;
    if (bVar11) {
      param_1 = param_1 + param_2 * -0x800000;
    }
    bVar12 = param_2 << 0x16 <= param_1;
    if (bVar12) {
      param_1 = param_1 + param_2 * -0x400000;
    }
    bVar13 = param_2 << 0x15 <= param_1;
    if (bVar13) {
      param_1 = param_1 + param_2 * -0x200000;
    }
    bVar14 = param_2 << 0x14 <= param_1;
    if (bVar14) {
      param_1 = param_1 + param_2 * -0x100000;
    }
    bVar15 = param_2 << 0x13 <= param_1;
    if (bVar15) {
      param_1 = param_1 + param_2 * -0x80000;
    }
    bVar16 = param_2 << 0x12 <= param_1;
    if (bVar16) {
      param_1 = param_1 + param_2 * -0x40000;
    }
    bVar17 = param_2 << 0x11 <= param_1;
    if (bVar17) {
      param_1 = param_1 + param_2 * -0x20000;
    }
    bVar18 = param_2 << 0x10 <= param_1;
    if (bVar18) {
      param_1 = param_1 + param_2 * -0x10000;
    }
    bVar19 = param_2 << 0xf <= param_1;
    if (bVar19) {
      param_1 = param_1 + param_2 * -0x8000;
    }
    bVar20 = param_2 << 0xe <= param_1;
    if (bVar20) {
      param_1 = param_1 + param_2 * -0x4000;
    }
    bVar21 = param_2 << 0xd <= param_1;
    if (bVar21) {
      param_1 = param_1 + param_2 * -0x2000;
    }
    bVar22 = param_2 << 0xc <= param_1;
    if (bVar22) {
      param_1 = param_1 + param_2 * -0x1000;
    }
    bVar23 = param_2 << 0xb <= param_1;
    if (bVar23) {
      param_1 = param_1 + param_2 * -0x800;
    }
    bVar24 = param_2 << 10 <= param_1;
    if (bVar24) {
      param_1 = param_1 + param_2 * -0x400;
    }
    bVar25 = param_2 << 9 <= param_1;
    if (bVar25) {
      param_1 = param_1 + param_2 * -0x200;
    }
    bVar26 = param_2 << 8 <= param_1;
    if (bVar26) {
      param_1 = param_1 + param_2 * -0x100;
    }
    bVar27 = param_2 << 7 <= param_1;
    if (bVar27) {
      param_1 = param_1 + param_2 * -0x80;
    }
    bVar28 = param_2 << 6 <= param_1;
    if (bVar28) {
      param_1 = param_1 + param_2 * -0x40;
    }
    bVar29 = param_2 << 5 <= param_1;
    if (bVar29) {
      param_1 = param_1 + param_2 * -0x20;
    }
    bVar30 = param_2 << 4 <= param_1;
    if (bVar30) {
      param_1 = param_1 + param_2 * -0x10;
    }
    bVar31 = param_2 << 3 <= param_1;
    if (bVar31) {
      param_1 = param_1 + param_2 * -8;
    }
    bVar32 = param_2 << 2 <= param_1;
    if (bVar32) {
      param_1 = param_1 + param_2 * -4;
    }
    bVar33 = param_2 << 1 <= param_1;
    if (bVar33) {
      param_1 = param_1 + param_2 * -2;
    }
    return CONCAT44(param_2,(((((((((((((((((((((((((((((((uint)bVar3 * 2 + (uint)bVar4) * 2 +
                                                        (uint)bVar5) * 2 + (uint)bVar6) * 2 +
                                                      (uint)bVar7) * 2 + (uint)bVar8) * 2 +
                                                    (uint)bVar9) * 2 + (uint)bVar10) * 2 +
                                                  (uint)bVar11) * 2 + (uint)bVar12) * 2 +
                                                (uint)bVar13) * 2 + (uint)bVar14) * 2 + (uint)bVar15
                                              ) * 2 + (uint)bVar16) * 2 + (uint)bVar17) * 2 +
                                           (uint)bVar18) * 2 + (uint)bVar19) * 2 + (uint)bVar20) * 2
                                        + (uint)bVar21) * 2 + (uint)bVar22) * 2 + (uint)bVar23) * 2
                                     + (uint)bVar24) * 2 + (uint)bVar25) * 2 + (uint)bVar26) * 2 +
                                  (uint)bVar27) * 2 + (uint)bVar28) * 2 + (uint)bVar29) * 2 +
                               (uint)bVar30) * 2 + (uint)bVar31) * 2 + (uint)bVar32) * 2 +
                            (uint)bVar33) * 2 + (uint)(param_2 <= param_1));
  }
                    /* WARNING: Could not recover jumptable at 0xa0004700. Too many branches */
                    /* WARNING: Treating indirect jump as call */
  uVar34 = (*(code *)(iVar2 * 0xc + -0x5fffb8f8))();
  return uVar34;
}



