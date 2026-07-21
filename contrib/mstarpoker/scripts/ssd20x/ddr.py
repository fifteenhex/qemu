#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
DDR bring-up for ssd20x targets - reusable by any script that needs DRAM.

init(lk) replays the captured vendor MIU register sequence
(ddr_seq_ssd202d) over an mstarpoker Link and polls for init/BIST
completion. See ddr_seq_ssd202d.py for how the sequence was captured and
why replaying it is faithful.
"""
import ddr_seq_ssd202d as seq

DRAM_BASE = 0x20000000


def init(lk, verbose=True):
    """Bring up DDR by replaying the MIU init sequence.

    Returns True only if every completion poll (MIU init-done, BIST)
    asserted. A False return means DRAM is *not* trained: callers must not
    touch 0x20000000+, because a read of untrained DRAM bus-hangs the CPU
    and cannot be recovered by the fault handler (only a reset clears it).
    """
    if verbose:
        print("[ddr] replaying %d MIU writes..." % len(seq.INIT_SEQ))
    ok = True
    for off, val in seq.INIT_SEQ:
        lk.write32(seq.MIU_BASE + off, val)
        poll = seq.POLL_AFTER.get((off, val))
        if poll:
            po, mask = poll
            for _ in range(2000):
                if lk.read32(seq.MIU_BASE + po) & mask:
                    break
            else:
                ok = False
                if verbose:
                    print("[ddr] WARNING: poll 0x%03x & 0x%04x timed out"
                          % (po, mask))
    if verbose:
        print("[ddr] sequence complete%s"
              % ("" if ok else " (with poll timeouts - DRAM NOT trained)"))
    return ok
