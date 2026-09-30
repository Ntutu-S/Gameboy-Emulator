#include "arm7tdmi.h"
#include "arm_instructions.h"
#include "thumb_instructions.h"
#include "bus.h"
#include "interrupt.h"
#include "gba.h"
#include <string.h>

/**
 * ARM7TDMI Processor Core Implementation
 * "Ntutu Advance System"
 */

void arm7tdmi_init(ARM7TDMI *cpu, GBABus *bus) {
    if (!cpu) return;
    memset(cpu, 0, sizeof(*cpu));
    cpu->bus = bus;
    arm7tdmi_reset(cpu);
}

void arm7tdmi_reset(ARM7TDMI *cpu) {
    if (!cpu) return;

    for (int i = 0; i < 16; i++) {
        cpu->r[i] = 0;
    }
    memset(&cpu->banks, 0, sizeof(cpu->banks));

    // Reset starts in Supervisor (SVC) mode, ARM state, IRQ & FIQ disabled
    cpu->cpsr = MODE_SVC | FLAG_I | FLAG_F;
    cpu->spsr = 0;
    cpu->cycles = 0;
    cpu->halted = false;
    cpu->branched = false;

    // Reset Vector 0x00000000
    cpu->r[REG_PC] = EX_RESET;
    if (cpu->bus) {
        arm7tdmi_fill_pipeline_arm(cpu, cpu->bus);
    }
}

ARMMode arm7tdmi_get_mode(const ARM7TDMI *cpu) {
    return (ARMMode)(cpu->cpsr & CPSR_MODE_MASK);
}

void arm7tdmi_switch_mode(ARM7TDMI *cpu, ARMMode new_mode) {
    if (!cpu) return;

    ARMMode old_mode = arm7tdmi_get_mode(cpu);
    if (old_mode == new_mode) {
        cpu->cpsr = (cpu->cpsr & ~CPSR_MODE_MASK) | (new_mode & CPSR_MODE_MASK);
        return;
    }

    ARMBankIndex old_bank = arm7tdmi_mode_to_bank(old_mode);
    ARMBankIndex new_bank = arm7tdmi_mode_to_bank(new_mode);

    // Save outgoing SP and LR
    cpu->banks.sp[old_bank] = cpu->r[REG_SP];
    cpu->banks.lr[old_bank] = cpu->r[REG_LR];

    if (old_bank != BANK_USR) {
        cpu->banks.spsr[old_bank] = cpu->spsr;
    }

    // Banked R8-R12 when entering or leaving FIQ mode
    if (old_mode == MODE_FIQ && new_mode != MODE_FIQ) {
        for (int i = 0; i < 5; i++) {
            cpu->banks.r8_12_fiq[i] = cpu->r[8 + i];
            cpu->r[8 + i] = cpu->banks.r8_12_usr[i];
        }
    } else if (old_mode != MODE_FIQ && new_mode == MODE_FIQ) {
        for (int i = 0; i < 5; i++) {
            cpu->banks.r8_12_usr[i] = cpu->r[8 + i];
            cpu->r[8 + i] = cpu->banks.r8_12_fiq[i];
        }
    }

    // Restore incoming SP and LR
    cpu->r[REG_SP] = cpu->banks.sp[new_bank];
    cpu->r[REG_LR] = cpu->banks.lr[new_bank];

    if (new_bank != BANK_USR) {
        cpu->spsr = cpu->banks.spsr[new_bank];
    } else {
        cpu->spsr = 0;
    }

    cpu->cpsr = (cpu->cpsr & ~CPSR_MODE_MASK) | (new_mode & CPSR_MODE_MASK);
}

void arm7tdmi_fill_pipeline_arm(ARM7TDMI *cpu, GBABus *bus) {
    if (!bus) return;
    cpu->r[REG_PC] &= ~3u;
    cpu->execute_addr = cpu->r[REG_PC];
    cpu->pipeline[0] = bus_read32(bus, cpu->r[REG_PC]);
    cpu->r[REG_PC] += 4;
    cpu->pipeline[1] = bus_read32(bus, cpu->r[REG_PC]);
    cpu->r[REG_PC] += 4;
    cpu->branched = false;
}

void arm7tdmi_fill_pipeline_thumb(ARM7TDMI *cpu, GBABus *bus) {
    if (!bus) return;
    cpu->r[REG_PC] &= ~1u;
    cpu->execute_addr = cpu->r[REG_PC];
    cpu->pipeline[0] = bus_read16(bus, cpu->r[REG_PC]);
    cpu->r[REG_PC] += 2;
    cpu->pipeline[1] = bus_read16(bus, cpu->r[REG_PC]);
    cpu->r[REG_PC] += 2;
    cpu->branched = false;
}

uint32_t arm7tdmi_get_reg(const ARM7TDMI *cpu, int reg) {
    if (!cpu || reg < 0 || reg > 15) return 0;
    return cpu->r[reg];
}

void arm7tdmi_set_reg(ARM7TDMI *cpu, int reg, uint32_t val) {
    if (!cpu || reg < 0 || reg > 15) return;
    if (reg == REG_PC) {
        if (arm_is_thumb(cpu)) {
            cpu->r[REG_PC] = val & ~1u;
            arm7tdmi_fill_pipeline_thumb(cpu, cpu->bus);
        } else {
            cpu->r[REG_PC] = val & ~3u;
            arm7tdmi_fill_pipeline_arm(cpu, cpu->bus);
        }
        cpu->branched = true;
    } else {
        cpu->r[reg] = val;
    }
}

void arm7tdmi_trigger_exception(ARM7TDMI *cpu, ARMException ex) {
    if (!cpu) return;

    uint32_t old_cpsr = cpu->cpsr;
    ARMMode target_mode;
    uint32_t lr_offset = 4;

    switch (ex) {
        case EX_RESET:
            target_mode = MODE_SVC;
            break;
        case EX_UNDEFINED_INST:
            target_mode = MODE_UND;
            lr_offset = arm_is_thumb(cpu) ? 2 : 4;
            break;
        case EX_SWI:
            target_mode = MODE_SVC;
            lr_offset = arm_is_thumb(cpu) ? 2 : 4;
            break;
        case EX_PREFETCH_ABORT:
            target_mode = MODE_ABT;
            lr_offset = 4;
            break;
        case EX_DATA_ABORT:
            target_mode = MODE_ABT;
            lr_offset = 8;
            break;
        case EX_IRQ:
            target_mode = MODE_IRQ;
            lr_offset = arm_is_thumb(cpu) ? 0 : 4;
            break;
        case EX_FIQ:
            target_mode = MODE_FIQ;
            lr_offset = arm_is_thumb(cpu) ? 0 : 4;
            break;
        default:
            target_mode = MODE_SVC;
            break;
    }

    // Compute LR return address
    uint32_t return_addr = cpu->r[REG_PC] - lr_offset;

    // Switch mode
    arm7tdmi_switch_mode(cpu, target_mode);

    // Set SPSR
    cpu->spsr = old_cpsr;
    ARMBankIndex bank = arm7tdmi_mode_to_bank(target_mode);
    if (bank != BANK_USR) {
        cpu->banks.spsr[bank] = old_cpsr;
    }

    cpu->r[REG_LR] = return_addr;

    // Switch to ARM mode & disable interrupts
    cpu->cpsr &= ~FLAG_T;
    cpu->cpsr |= FLAG_I;
    if (ex == EX_RESET || ex == EX_FIQ) {
        cpu->cpsr |= FLAG_F;
    }

    cpu->r[REG_PC] = (uint32_t)ex;
    arm7tdmi_fill_pipeline_arm(cpu, cpu->bus);
    cpu->branched = true;
}

int arm7tdmi_step(ARM7TDMI *cpu) {
    if (!cpu || !cpu->bus) return 1;

    // Handle HALT state
    if (cpu->halted) {
        // Unhalt condition checked by interrupt system
        return 1;
    }

    // Check IRQ trigger
    if (cpu->bus->gba && !(cpu->cpsr & FLAG_I)) {
        if (interrupt_check_pending(&cpu->bus->gba->intr)) {
            interrupt_dispatch(&cpu->bus->gba->intr);
            return 3;
        }
    }

    cpu->branched = false;

    if (arm_is_thumb(cpu)) {
        uint16_t opcode = (uint16_t)cpu->pipeline[0];
        cpu->execute_addr = cpu->r[REG_PC] - 4;

        int cycles = thumb_execute_instruction(cpu, opcode);
        cpu->cycles += cycles;

        if (!cpu->branched) {
            cpu->pipeline[0] = cpu->pipeline[1];
            cpu->pipeline[1] = bus_read16(cpu->bus, cpu->r[REG_PC]);
            cpu->r[REG_PC] += 2;
        }
        return cycles;
    } else {
        uint32_t opcode = cpu->pipeline[0];
        cpu->execute_addr = cpu->r[REG_PC] - 8;

        int cycles = arm_execute_instruction(cpu, opcode);
        cpu->cycles += cycles;

        if (!cpu->branched) {
            cpu->pipeline[0] = cpu->pipeline[1];
            cpu->pipeline[1] = bus_read32(cpu->bus, cpu->r[REG_PC]);
            cpu->r[REG_PC] += 4;
        }
        return cycles;
    }
}
