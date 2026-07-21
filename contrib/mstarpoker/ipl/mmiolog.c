/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal TCG plugin: log MMIO stores as "pc addr size value" */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static FILE *out;
static uint64_t lo = 0x1f000000, hi = 0x20000000; /* RIU MMIO window */

static void mem_cb(unsigned int cpu, qemu_plugin_meminfo_t info,
                   uint64_t vaddr, void *ud)
{
    if (vaddr < lo || vaddr >= hi) {
        return;
    }
    uint64_t pc = (uint64_t)(uintptr_t)ud;
    qemu_plugin_mem_value v = qemu_plugin_mem_get_value(info);
    uint64_t val = 0; unsigned bytes = 0;
    switch (v.type) {
    case QEMU_PLUGIN_MEM_VALUE_U8:  val = v.data.u8;  bytes = 1; break;
    case QEMU_PLUGIN_MEM_VALUE_U16: val = v.data.u16; bytes = 2; break;
    case QEMU_PLUGIN_MEM_VALUE_U32: val = v.data.u32; bytes = 4; break;
    case QEMU_PLUGIN_MEM_VALUE_U64: val = v.data.u64; bytes = 8; break;
    default: return;
    }
    fprintf(out, "%08" PRIx64 " %08" PRIx64 " %u %0*" PRIx64 "\n",
            pc, vaddr, bytes, (int)(bytes * 2), val);
}

static void tb_trans(struct qemu_plugin_tb *tb, void *ud)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t pc = qemu_plugin_insn_vaddr(insn);
        qemu_plugin_register_vcpu_mem_cb(insn, mem_cb,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_W,
                                         (void *)(uintptr_t)pc);
    }
}

static void at_exit(void *p)
{
    if (out) { fflush(out); fclose(out); out = NULL; }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
        const qemu_info_t *info, int argc, char **argv)
{
    const char *path = "/tmp/mmiolog.txt";
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "out=", 4)) path = argv[i] + 4;
        else if (!strncmp(argv[i], "lo=", 3)) lo = strtoull(argv[i] + 3, 0, 0);
        else if (!strncmp(argv[i], "hi=", 3)) hi = strtoull(argv[i] + 3, 0, 0);
    }
    out = fopen(path, "w");
    if (!out) return -1;
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
