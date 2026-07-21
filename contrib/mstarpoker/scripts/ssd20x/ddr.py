#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
DDR bring-up for ssd20x targets - reusable by any script that needs DRAM.

Two methods:

* init(lk) / init_c(lk) - THE real trainer. Uploads the bare-metal DDR-init
  blob (ddr_c/ddr_init.bin, a clean C reimplementation of the vendor IPL's DDR
  logic) into SRAM and runs it on-target via the stub's 'G'. Because it runs on
  the chip, its read-modify-write ZQ / drive-strength calibration works against
  the real PHY and its settle delays use the real timer - so it actually trains
  real DRAM. See ddr_c/ddr_init.c.

* init_replay(lk) - replays the captured MIU register writes
  (ddr_seq_ssd202d) over the serial link. This does NOT train real DRAM (it
  drops the delays and skips the read-dependent ZQ calibration); it only
  exercises the flow under QEMU, whose MIU is a stub. Kept for reference and
  as a QEMU-only fallback. See VALIDATION.md for why replay is insufficient.

init(lk) returns True only if DRAM genuinely trained (MIU init-done + BIST
asserted). A False return means callers must not touch 0x20000000+, because a
read of untrained DRAM bus-hangs the CPU with no recoverable abort.
"""
import os

import ddr_seq_ssd202d as seq

DRAM_BASE = 0x20000000

# The DDR-init blob and where it is uploaded/run (matches ddr_c/ddr.ld).
DDR_BLOB = os.path.join(os.path.dirname(__file__), "ddr_c", "ddr_init.bin")
BLOB_ADDR = 0xa0008000


def _poll(lk, off, mask, tries=2000):
    for _ in range(tries):
        if lk.read32(seq.MIU_BASE + off) & mask:
            return True
    return False


def _trained(lk):
    """True if the MIU reports init-done and BIST-done (DRAM is up)."""
    return _poll(lk, 0x400, 0x8000) and _poll(lk, 0x5c0, 0x8000)


def init_c(lk, verbose=True, blob=DDR_BLOB):
    """Upload and run the on-target DDR-init blob. Returns the trained flag."""
    with open(blob, "rb") as f:
        data = f.read()
    if verbose:
        print("[ddr] uploading DDR-init blob (%d bytes) to 0x%08x"
              % (len(data), BLOB_ADDR))
    lk.upload(BLOB_ADDR, data)
    if verbose:
        print("[ddr] running on-target ddr_init() (real ZQ cal + delays)...")
    # The blob busy-waits through the DDR settle delays before returning.
    lk.go(BLOB_ADDR, timeout=5.0)
    if not lk.alive():
        if verbose:
            print("[ddr] stub did not come back after ddr_init - target wedged")
        return False
    ok = _trained(lk)
    if verbose:
        print("[ddr] ddr_init done%s"
              % ("" if ok else " (init-done/BIST NOT set - DRAM not trained)"))
    return ok


def init_replay(lk, verbose=True):
    """Replay the captured MIU writes (QEMU-only; does not train real DRAM)."""
    if verbose:
        print("[ddr] replaying %d MIU writes (QEMU flow only)..."
              % len(seq.INIT_SEQ))
    ok = True
    for off, val in seq.INIT_SEQ:
        lk.write32(seq.MIU_BASE + off, val)
        poll = seq.POLL_AFTER.get((off, val))
        if poll:
            po, mask = poll
            if not _poll(lk, po, mask):
                ok = False
                if verbose:
                    print("[ddr] WARNING: poll 0x%03x & 0x%04x timed out"
                          % (po, mask))
    if verbose:
        print("[ddr] replay complete%s"
              % ("" if ok else " (poll timeouts - DRAM NOT trained)"))
    return ok


def init(lk, verbose=True, method="c"):
    """Bring up DDR. method='c' runs the on-target blob (real trainer);
    method='replay' uses the QEMU-only register replay. Falls back to replay
    if the blob is missing. Returns the trained flag."""
    if method == "c" and os.path.exists(DDR_BLOB):
        return init_c(lk, verbose=verbose)
    if method == "c" and verbose:
        print("[ddr] %s not built (run 'make' in ddr_c/); using replay"
              % os.path.basename(DDR_BLOB))
    return init_replay(lk, verbose=verbose)
