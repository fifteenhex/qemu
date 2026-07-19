/*
 * memwatch - a data watchpoint that works without gdb.
 *
 * Logs every guest memory access to a target address (or range), with the PC of
 * the accessing instruction. Built for RE'ing firmware where the gdbstub is
 * unusable (e.g. the mercury5 target: broken g-packet). Answers "is this flag
 * ever written, and by whom" - which gdb watchpoints would normally give.
 *
 *   -plugin .../libmemwatch.so,addr=0x204d6424            # 8 bytes at addr
 *   -plugin .../libmemwatch.so,addr=0x204d6424,end=0x204d6430,reads=1
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t watch_lo, watch_hi;
static bool watch_reads;

static void vcpu_mem(unsigned int cpu, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    if (vaddr < watch_lo || vaddr > watch_hi) {
        return;
    }
    bool store = qemu_plugin_mem_is_store(info);
    if (!store && !watch_reads) {
        return;
    }
    fprintf(stderr, "[memwatch] %-5s addr=0x%08"PRIx64" size=%u pc=0x%08"PRIx64"\n",
            store ? "WRITE" : "read", vaddr,
            1u << qemu_plugin_mem_size_shift(info),
            (uint64_t)(uintptr_t)udata);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *ud)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t pc = qemu_plugin_insn_vaddr(insn);
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem, QEMU_PLUGIN_CB_NO_REGS,
                                         watch_reads ? QEMU_PLUGIN_MEM_RW
                                                     : QEMU_PLUGIN_MEM_W,
                                         (void *)(uintptr_t)pc);
    }
}

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) t = g_strsplit(argv[i], "=", 2);
        if (!strcmp(t[0], "addr")) {
            watch_lo = watch_hi = g_ascii_strtoull(t[1], NULL, 0);
        } else if (!strcmp(t[0], "end")) {
            watch_hi = g_ascii_strtoull(t[1], NULL, 0);
        } else if (!strcmp(t[0], "reads")) {
            watch_reads = !t[1] || t[1][0] != '0';
        }
    }
    if (!watch_lo) {
        fprintf(stderr, "memwatch: need addr=0x...\n");
        return -1;
    }
    if (watch_hi < watch_lo) {
        watch_hi = watch_lo + 7;
    }
    fprintf(stderr, "[memwatch] watching 0x%08"PRIx64"..0x%08"PRIx64" (%s)\n",
            watch_lo, watch_hi, watch_reads ? "r/w" : "writes");
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
