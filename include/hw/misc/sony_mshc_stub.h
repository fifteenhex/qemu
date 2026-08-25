/*
 * Sony CLIE Memory Stick host controller (Fujitsu MB86189) -- Phase 1
 * "no card present" stub.
 *
 * The real chip is a 0x14-byte register file (MSCMD/MSCS/MSDATA/MSICS/
 * MSPPCD) documented by Cloudpilot-emu's EmRegsMB86189; see
 * CLIE-RESEARCH.md sec 4.3 / sec 7 and CLIE-POC-NOTES.md for the trace
 * that pinned down the register layout and the PEG-S300 base address
 * (0x10200000, from EmDevice.cpp).  This stub answers MSCS as
 * permanently idle (matches the real chip's post-reset value) and
 * everything else as zero/no-op, which is enough for the Expansion
 * Manager to see an empty slot and move on.  A functional model
 * (TPC protocol, card image) is Phase 3.
 */

#ifndef HW_MISC_SONY_MSHC_STUB_H
#define HW_MISC_SONY_MSHC_STUB_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SONY_MSHC_STUB "sony-clie-mshc-stub"

typedef struct SonyMshcStubState SonyMshcStubState;
OBJECT_DECLARE_SIMPLE_TYPE(SonyMshcStubState, SONY_MSHC_STUB)

struct SonyMshcStubState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
};

#endif
