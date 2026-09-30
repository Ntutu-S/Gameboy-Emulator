#include "interrupt.h"
#include "gba.h"
#include <string.h>

/**
 * Interrupt Controller Subsystem
 * "Ntutu Advance System"
 */

void interrupt_init(Interrupt *intr, struct GBA *gba) {
    if (!intr) return;
    memset(intr, 0, sizeof(*intr));
    intr->gba = gba;
    interrupt_reset(intr);
}

void interrupt_reset(Interrupt *intr) {
    if (!intr) return;
    intr->ime = 0;
    intr->ie = 0;
    intr->if_ = 0;
}

void interrupt_raise(Interrupt *intr, uint16_t mask) {
    if (!intr) return;
    intr->if_ |= mask;

    // CRITICAL: HALT Wake-Up Condition
    // The CPU unhalts ONLY when an interrupt enabled in IE is signaled:
    // Unhalt Condition = (IE & IF) != 0
    if (intr->gba && intr->gba->cpu.halted) {
        if ((intr->ie & intr->if_) != 0) {
            intr->gba->cpu.halted = false;
        }
    }
}

bool interrupt_check_pending(Interrupt *intr) {
    if (!intr) return false;

    // Check if any enabled interrupt is active
    uint16_t active = intr->ie & intr->if_;

    // Handle CPU unhalt check
    if (active != 0 && intr->gba && intr->gba->cpu.halted) {
        intr->gba->cpu.halted = false;
    }

    // Only dispatch to CPU if Master Enable (IME) is set
    return (intr->ime & 1) && (active != 0);
}

void interrupt_dispatch(Interrupt *intr) {
    if (!intr || !intr->gba) return;

    // On every IRQ dispatch, OR triggered interrupt bits into BIOS check flags at IWRAM 0x03007FF8
    uint16_t active = intr->ie & intr->if_;
    if (active != 0) {
        *(uint16_t*)&intr->gba->bus.iwram[0x7FF8] |= active;
    }

    arm7tdmi_trigger_exception(&intr->gba->cpu, EX_IRQ);
}
