#include "thumb_instructions.h"
#include "arm_instructions.h"
#include "bus.h"
#include "bios.h"
#include "gba.h"

/**
 * 16-bit THUMB Instruction Set Execution (Formats 1 - 19)
 * "Ntutu Advance System"
 */

static inline void set_thumb_nz(ARM7TDMI *cpu, uint32_t val) {
    arm_set_flag(cpu, FLAG_N, (val & 0x80000000u) != 0);
    arm_set_flag(cpu, FLAG_Z, val == 0);
}

static inline uint32_t thumb_add(ARM7TDMI *cpu, uint32_t a, uint32_t b, bool carry_in, bool set_flags) {
    uint64_t res = (uint64_t)a + (uint64_t)b + (carry_in ? 1 : 0);
    uint32_t r = (uint32_t)res;
    if (set_flags) {
        set_thumb_nz(cpu, r);
        arm_set_flag(cpu, FLAG_C, res > 0xFFFFFFFFull);
        arm_set_flag(cpu, FLAG_V, (~(a ^ b) & (a ^ r) & 0x80000000u) != 0);
    }
    return r;
}

static inline uint32_t thumb_sub(ARM7TDMI *cpu, uint32_t a, uint32_t b, bool carry_in, bool set_flags) {
    uint64_t res = (uint64_t)a - (uint64_t)b - (carry_in ? 0 : 1);
    uint32_t r = (uint32_t)res;
    if (set_flags) {
        set_thumb_nz(cpu, r);
        arm_set_flag(cpu, FLAG_C, (uint64_t)a >= ((uint64_t)b + (carry_in ? 0 : 1)));
        arm_set_flag(cpu, FLAG_V, ((a ^ b) & (a ^ r) & 0x80000000u) != 0);
    }
    return r;
}

int thumb_execute_instruction(ARM7TDMI *cpu, uint16_t opcode) {
    GBABus *bus = cpu->bus;

    // Format 1: Move Shifted Register (LSL, LSR, ASR)
    if ((opcode & 0xE000) == 0x0000 && (opcode & 0x1800) != 0x1800) {
        uint32_t shift_op = (opcode >> 11) & 3;
        uint32_t offset5 = (opcode >> 6) & 0x1F;
        int rs = (opcode >> 3) & 7;
        int rd = opcode & 7;

        uint32_t val = cpu->r[rs];
        uint32_t res = arm_shift(cpu, val, shift_op, offset5, true);
        cpu->r[rd] = res;
        set_thumb_nz(cpu, res);
        return 1;
    }

    // Format 2: Add / Subtract (Register or 3-bit immediate)
    if ((opcode & 0xF800) == 0x1800) {
        bool is_imm = (opcode & (1 << 10)) != 0;
        bool is_sub = (opcode & (1 << 9)) != 0;
        int rn_or_imm = (opcode >> 6) & 7;
        int rs = (opcode >> 3) & 7;
        int rd = opcode & 7;

        uint32_t op1 = cpu->r[rs];
        uint32_t op2 = is_imm ? (uint32_t)rn_or_imm : cpu->r[rn_or_imm];
        uint32_t res;

        if (is_sub) {
            res = thumb_sub(cpu, op1, op2, true, true);
        } else {
            res = thumb_add(cpu, op1, op2, false, true);
        }
        cpu->r[rd] = res;
        return 1;
    }

    // Format 3: Move / Compare / Add / Subtract Immediate (8-bit immediate)
    if ((opcode & 0xE000) == 0x2000) {
        uint32_t op = (opcode >> 11) & 3;
        int rd = (opcode >> 8) & 7;
        uint32_t imm = opcode & 0xFF;

        switch (op) {
            case 0: // MOV
                cpu->r[rd] = imm;
                set_thumb_nz(cpu, imm);
                break;
            case 1: // CMP
                thumb_sub(cpu, cpu->r[rd], imm, true, true);
                break;
            case 2: // ADD
                cpu->r[rd] = thumb_add(cpu, cpu->r[rd], imm, false, true);
                break;
            case 3: // SUB
                cpu->r[rd] = thumb_sub(cpu, cpu->r[rd], imm, true, true);
                break;
        }
        return 1;
    }

    // Format 4: ALU Operations
    if ((opcode & 0xFC00) == 0x4000) {
        uint32_t op = (opcode >> 6) & 0xF;
        int rs = (opcode >> 3) & 7;
        int rd = opcode & 7;
        uint32_t op1 = cpu->r[rd];
        uint32_t op2 = cpu->r[rs];
        uint32_t res = 0;

        switch (op) {
            case 0x0: // AND
                res = op1 & op2;
                cpu->r[rd] = res;
                set_thumb_nz(cpu, res);
                break;
            case 0x1: // EOR
                res = op1 ^ op2;
                cpu->r[rd] = res;
                set_thumb_nz(cpu, res);
                break;
            case 0x2: // LSL
                res = arm_shift(cpu, op1, 0, op2 & 0xFF, true);
                cpu->r[rd] = res;
                set_thumb_nz(cpu, res);
                break;
            case 0x3: // LSR
                res = arm_shift(cpu, op1, 1, op2 & 0xFF, true);
                cpu->r[rd] = res;
                set_thumb_nz(cpu, res);
                break;
            case 0x4: // ASR
                res = arm_shift(cpu, op1, 2, op2 & 0xFF, true);
                cpu->r[rd] = res;
                set_thumb_nz(cpu, res);
                break;
            case 0x5: // ADC
                res = thumb_add(cpu, op1, op2, arm_get_flag(cpu, FLAG_C), true);
                cpu->r[rd] = res;
                break;
            case 0x6: // SBC
                res = thumb_sub(cpu, op1, op2, arm_get_flag(cpu, FLAG_C), true);
                cpu->r[rd] = res;
                break;
            case 0x7: // ROR
                res = arm_shift(cpu, op1, 3, op2 & 0xFF, true);
                cpu->r[rd] = res;
                set_thumb_nz(cpu, res);
                break;
            case 0x8: // TST
                res = op1 & op2;
                set_thumb_nz(cpu, res);
                break;
            case 0x9: // NEG
                res = thumb_sub(cpu, 0, op2, true, true);
                cpu->r[rd] = res;
                break;
            case 0xA: // CMP
                thumb_sub(cpu, op1, op2, true, true);
                break;
            case 0xB: // CMN
                thumb_add(cpu, op1, op2, false, true);
                break;
            case 0xC: // ORR
                res = op1 | op2;
                cpu->r[rd] = res;
                set_thumb_nz(cpu, res);
                break;
            case 0xD: // MUL
                res = op1 * op2;
                cpu->r[rd] = res;
                set_thumb_nz(cpu, res);
                break;
            case 0xE: // BIC
                res = op1 & ~op2;
                cpu->r[rd] = res;
                set_thumb_nz(cpu, res);
                break;
            case 0xF: // MVN
                res = ~op2;
                cpu->r[rd] = res;
                set_thumb_nz(cpu, res);
                break;
        }
        return 1;
    }

    // Format 5: Hi Register Operations / Branch Exchange
    if ((opcode & 0xFC00) == 0x4400) {
        uint32_t op = (opcode >> 8) & 3;
        bool h1 = (opcode & (1 << 7)) != 0;
        bool h2 = (opcode & (1 << 6)) != 0;
        int rs = ((opcode >> 3) & 7) | (h2 ? 8 : 0);
        int rd = (opcode & 7) | (h1 ? 8 : 0);

        if (op == 3) {
            // BX Rs
            uint32_t target = cpu->r[rs];
            if (target & 1) {
                // Stay in THUMB mode
                cpu->r[REG_PC] = target & ~1u;
                arm7tdmi_fill_pipeline_thumb(cpu, bus);
            } else {
                // Switch to ARM mode
                cpu->cpsr &= ~FLAG_T;
                cpu->r[REG_PC] = target & ~3u;
                arm7tdmi_fill_pipeline_arm(cpu, bus);
            }
            cpu->branched = true;
            return 3;
        }

        uint32_t op1 = cpu->r[rd];
        uint32_t op2 = cpu->r[rs];

        switch (op) {
            case 0: // ADD
                cpu->r[rd] = op1 + op2;
                if (rd == REG_PC) {
                    cpu->r[REG_PC] &= ~1u;
                    arm7tdmi_fill_pipeline_thumb(cpu, bus);
                    cpu->branched = true;
                }
                break;
            case 1: // CMP
                thumb_sub(cpu, op1, op2, true, true);
                break;
            case 2: // MOV
                cpu->r[rd] = op2;
                if (rd == REG_PC) {
                    cpu->r[REG_PC] &= ~1u;
                    arm7tdmi_fill_pipeline_thumb(cpu, bus);
                    cpu->branched = true;
                }
                break;
        }
        return (rd == REG_PC) ? 3 : 1;
    }

    // Format 6: PC-Relative Load (LDR Rd, [PC, #imm8 * 4])
    if ((opcode & 0xF800) == 0x4800) {
        int rd = (opcode >> 8) & 7;
        uint32_t imm = (opcode & 0xFF) * 4;
        uint32_t addr = (cpu->execute_addr & ~3u) + 4 + imm;
        cpu->r[rd] = bus_read32(bus, addr);
        return 2;
    }

    // Format 7: Load/Store with Register Offset
    if ((opcode & 0xF200) == 0x5000) {
        bool byte_trans = (opcode & (1 << 10)) != 0;
        bool load = (opcode & (1 << 11)) != 0;
        int ro = (opcode >> 6) & 7;
        int rb = (opcode >> 3) & 7;
        int rd = opcode & 7;
        uint32_t addr = cpu->r[rb] + cpu->r[ro];

        if (load) {
            cpu->r[rd] = byte_trans ? bus_read8(bus, addr) : bus_read32(bus, addr);
        } else {
            if (byte_trans) bus_write8(bus, addr, (uint8_t)cpu->r[rd]);
            else bus_write32(bus, addr, cpu->r[rd]);
        }
        return 2;
    }

    // Format 8: Load/Store Sign-Extended Byte/Halfword
    if ((opcode & 0xF200) == 0x5200) {
        uint32_t op = (opcode >> 10) & 3;
        int ro = (opcode >> 6) & 7;
        int rb = (opcode >> 3) & 7;
        int rd = opcode & 7;
        uint32_t addr = cpu->r[rb] + cpu->r[ro];

        switch (op) {
            case 0: // STRH
                bus_write16(bus, addr, (uint16_t)cpu->r[rd]);
                break;
            case 1: // LDSB
                cpu->r[rd] = (int32_t)(int8_t)bus_read8(bus, addr);
                break;
            case 2: // LDRH
                cpu->r[rd] = bus_read16(bus, addr);
                break;
            case 3: // LDSH
                if (addr & 1) cpu->r[rd] = (int32_t)(int8_t)bus_read8(bus, addr);
                else cpu->r[rd] = (int32_t)(int16_t)bus_read16(bus, addr);
                break;
        }
        return 2;
    }

    // Format 9: Load/Store with Immediate Offset
    if ((opcode & 0xE000) == 0x6000) {
        bool byte_trans = (opcode & (1 << 12)) != 0;
        bool load = (opcode & (1 << 11)) != 0;
        uint32_t offset = (opcode >> 6) & 0x1F;
        int rb = (opcode >> 3) & 7;
        int rd = opcode & 7;
        uint32_t addr = cpu->r[rb] + (byte_trans ? offset : (offset * 4));

        if (load) {
            cpu->r[rd] = byte_trans ? bus_read8(bus, addr) : bus_read32(bus, addr);
        } else {
            if (byte_trans) bus_write8(bus, addr, (uint8_t)cpu->r[rd]);
            else bus_write32(bus, addr, cpu->r[rd]);
        }
        return 2;
    }

    // Format 10: Load/Store Halfword Immediate
    if ((opcode & 0xF000) == 0x8000) {
        bool load = (opcode & (1 << 11)) != 0;
        uint32_t offset = ((opcode >> 6) & 0x1F) * 2;
        int rb = (opcode >> 3) & 7;
        int rd = opcode & 7;
        uint32_t addr = cpu->r[rb] + offset;

        if (load) {
            cpu->r[rd] = bus_read16(bus, addr);
        } else {
            bus_write16(bus, addr, (uint16_t)cpu->r[rd]);
        }
        return 2;
    }

    // Format 11: SP-Relative Load/Store
    if ((opcode & 0xF000) == 0x9000) {
        bool load = (opcode & (1 << 11)) != 0;
        int rd = (opcode >> 8) & 7;
        uint32_t offset = (opcode & 0xFF) * 4;
        uint32_t addr = cpu->r[REG_SP] + offset;

        if (load) {
            cpu->r[rd] = bus_read32(bus, addr);
        } else {
            bus_write32(bus, addr, cpu->r[rd]);
        }
        return 2;
    }

    // Format 12: Load Address (ADD Rd, PC/SP, #imm8 * 4)
    if ((opcode & 0xF000) == 0xA000) {
        bool use_sp = (opcode & (1 << 11)) != 0;
        int rd = (opcode >> 8) & 7;
        uint32_t imm = (opcode & 0xFF) * 4;
        if (use_sp) {
            cpu->r[rd] = cpu->r[REG_SP] + imm;
        } else {
            cpu->r[rd] = (cpu->execute_addr & ~3u) + 4 + imm;
        }
        return 1;
    }

    // Format 13: Add Offset to Stack Pointer
    if ((opcode & 0xFF00) == 0xB000) {
        bool is_neg = (opcode & (1 << 7)) != 0;
        uint32_t imm = (opcode & 0x7F) * 4;
        if (is_neg) {
            cpu->r[REG_SP] -= imm;
        } else {
            cpu->r[REG_SP] += imm;
        }
        return 1;
    }

    // Format 14: Push / Pop Registers
    if ((opcode & 0xF600) == 0xB400) {
        bool load = (opcode & (1 << 11)) != 0;
        bool pc_lr = (opcode & (1 << 8)) != 0;
        uint8_t rlist = opcode & 0xFF;

        if (!load) {
            // PUSH
            int count = (pc_lr ? 1 : 0);
            for (int i = 0; i < 8; i++) {
                if (rlist & (1 << i)) count++;
            }
            uint32_t addr = cpu->r[REG_SP] - count * 4;
            cpu->r[REG_SP] = addr;

            for (int i = 0; i < 8; i++) {
                if (rlist & (1 << i)) {
                    bus_write32(bus, addr, cpu->r[i]);
                    addr += 4;
                }
            }
            if (pc_lr) {
                bus_write32(bus, addr, cpu->r[REG_LR]);
            }
        } else {
            // POP
            uint32_t addr = cpu->r[REG_SP];
            for (int i = 0; i < 8; i++) {
                if (rlist & (1 << i)) {
                    cpu->r[i] = bus_read32(bus, addr);
                    addr += 4;
                }
            }
            if (pc_lr) {
                uint32_t target = bus_read32(bus, addr);
                addr += 4;
                cpu->r[REG_PC] = target & ~1u;
                arm7tdmi_fill_pipeline_thumb(cpu, bus);
                cpu->branched = true;
            }
            cpu->r[REG_SP] = addr;
        }
        return 2;
    }

    // Format 15: Multiple Load / Store (STMIA, LDMIA)
    if ((opcode & 0xF000) == 0xC000) {
        bool load = (opcode & (1 << 11)) != 0;
        int rb = (opcode >> 8) & 7;
        uint8_t rlist = opcode & 0xFF;
        uint32_t addr = cpu->r[rb];

        for (int i = 0; i < 8; i++) {
            if (rlist & (1 << i)) {
                if (load) {
                    cpu->r[i] = bus_read32(bus, addr);
                } else {
                    bus_write32(bus, addr, cpu->r[i]);
                }
                addr += 4;
            }
        }
        cpu->r[rb] = addr;
        return 2;
    }

    // Format 17: Software Interrupt (SWI)
    if ((opcode & 0xFF00) == 0xDF00) {
        uint8_t swi_num = opcode & 0xFF;
        if (cpu->bus && cpu->bus->gba) {
            bios_call(cpu->bus->gba, swi_num);
        } else {
            arm7tdmi_trigger_exception(cpu, EX_SWI);
        }
        return 3;
    }

    // Format 16: Conditional Branch (B<cond>)
    if ((opcode & 0xF000) == 0xD000) {
        uint32_t cond = (opcode >> 8) & 0xF;
        if (arm7tdmi_check_condition(cpu, cond)) {
            int32_t offset = (int8_t)(opcode & 0xFF);
            offset <<= 1;
            cpu->r[REG_PC] = (uint32_t)((int32_t)cpu->r[REG_PC] + offset);
            arm7tdmi_fill_pipeline_thumb(cpu, bus);
            cpu->branched = true;
            return 3;
        }
        return 1;
    }

    // Format 18: Unconditional Branch (B)
    if ((opcode & 0xF800) == 0xE000) {
        int32_t offset = (int32_t)(opcode & 0x7FF);
        if (offset & 0x400) offset |= 0xFFFFF800; // Sign extend 11-bit
        offset <<= 1;
        cpu->r[REG_PC] = (uint32_t)((int32_t)cpu->r[REG_PC] + offset);
        arm7tdmi_fill_pipeline_thumb(cpu, bus);
        cpu->branched = true;
        return 3;
    }

    // Format 19: Long Branch with Link (BL)
    if ((opcode & 0xF000) == 0xF000) {
        bool is_suffix = (opcode & (1 << 11)) != 0;
        uint32_t offset = opcode & 0x7FF;

        if (!is_suffix) {
            // First instruction (Prefix)
            int32_t off = (int32_t)offset;
            if (off & 0x400) off |= 0xFFFFF800;
            cpu->r[REG_LR] = cpu->r[REG_PC] + (off << 12);
            return 1;
        } else {
            // Second instruction (Suffix)
            uint32_t next_pc = cpu->r[REG_LR] + (offset << 1);
            cpu->r[REG_LR] = (cpu->execute_addr + 2) | 1;
            cpu->r[REG_PC] = next_pc & ~1u;
            arm7tdmi_fill_pipeline_thumb(cpu, bus);
            cpu->branched = true;
            return 3;
        }
    }

    return 1;
}
