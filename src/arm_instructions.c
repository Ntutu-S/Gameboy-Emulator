#include "arm_instructions.h"
#include "bus.h"
#include "bios.h"
#include "gba.h"
#include <stdio.h>

/**
 * 32-bit ARMv4T Instruction Set Execution
 * "Ntutu Advance System"
 */

bool arm7tdmi_check_condition(const ARM7TDMI *cpu, uint32_t cond) {
    bool n = (cpu->cpsr & FLAG_N) != 0;
    bool z = (cpu->cpsr & FLAG_Z) != 0;
    bool c = (cpu->cpsr & FLAG_C) != 0;
    bool v = (cpu->cpsr & FLAG_V) != 0;

    switch (cond) {
        case 0x0: return z;                       // EQ - Equal (Z set)
        case 0x1: return !z;                      // NE - Not equal (Z clear)
        case 0x2: return c;                       // CS/HS - Carry set / Unsigned higher or same
        case 0x3: return !c;                      // CC/LO - Carry clear / Unsigned lower
        case 0x4: return n;                       // MI - Minus / Negative (N set)
        case 0x5: return !n;                      // PL - Plus / Positive (N clear)
        case 0x6: return v;                       // VS - Overflow (V set)
        case 0x7: return !v;                      // VC - No overflow (V clear)
        case 0x8: return c && !z;                 // HI - Unsigned higher
        case 0x9: return !c || z;                 // LS - Unsigned lower or same
        case 0xA: return n == v;                  // GE - Signed greater than or equal
        case 0xB: return n != v;                  // LT - Signed less than
        case 0xC: return !z && (n == v);          // GT - Signed greater than
        case 0xD: return z || (n != v);           // LE - Signed less than or equal
        case 0xE: return true;                    // AL - Always
        case 0xF: return false;                   // NV - Never (reserved)
        default:  return false;
    }
}

uint32_t arm_shift(ARM7TDMI *cpu, uint32_t val, uint32_t shift_type, uint32_t amount, bool update_carry) {
    if (amount == 0) {
        // Shift amount of 0 behavior
        switch (shift_type) {
            case 0: // LSL #0: No shift, carry unchanged
                return val;
            case 1: // LSR #0: Special encoding for LSR #32
                if (update_carry) {
                    arm_set_flag(cpu, FLAG_C, (val & 0x80000000u) != 0);
                }
                return 0;
            case 2: // ASR #0: Special encoding for ASR #32
                if (update_carry) {
                    arm_set_flag(cpu, FLAG_C, (val & 0x80000000u) != 0);
                }
                return (val & 0x80000000u) ? 0xFFFFFFFFu : 0;
            case 3: // ROR #0: Special encoding for RRX (Rotate Right with Extend by 1)
                {
                    bool old_carry = arm_get_flag(cpu, FLAG_C);
                    if (update_carry) {
                        arm_set_flag(cpu, FLAG_C, (val & 1) != 0);
                    }
                    return (val >> 1) | ((uint32_t)old_carry << 31);
                }
            default:
                return val;
        }
    }

    switch (shift_type) {
        case 0: // LSL - Logical Shift Left
            if (amount < 32) {
                if (update_carry) {
                    arm_set_flag(cpu, FLAG_C, ((val >> (32 - amount)) & 1) != 0);
                }
                return val << amount;
            } else if (amount == 32) {
                if (update_carry) {
                    arm_set_flag(cpu, FLAG_C, (val & 1) != 0);
                }
                return 0;
            } else {
                if (update_carry) {
                    arm_set_flag(cpu, FLAG_C, false);
                }
                return 0;
            }

        case 1: // LSR - Logical Shift Right
            if (amount < 32) {
                if (update_carry) {
                    arm_set_flag(cpu, FLAG_C, ((val >> (amount - 1)) & 1) != 0);
                }
                return val >> amount;
            } else if (amount == 32) {
                if (update_carry) {
                    arm_set_flag(cpu, FLAG_C, (val & 0x80000000u) != 0);
                }
                return 0;
            } else {
                if (update_carry) {
                    arm_set_flag(cpu, FLAG_C, false);
                }
                return 0;
            }

        case 2: // ASR - Arithmetic Shift Right
            if (amount < 32) {
                if (update_carry) {
                    arm_set_flag(cpu, FLAG_C, (((int32_t)val >> (amount - 1)) & 1) != 0);
                }
                return (uint32_t)((int32_t)val >> amount);
            } else {
                bool bit31 = (val & 0x80000000u) != 0;
                if (update_carry) {
                    arm_set_flag(cpu, FLAG_C, bit31);
                }
                return bit31 ? 0xFFFFFFFFu : 0;
            }

        case 3: // ROR - Rotate Right
            amount %= 32;
            if (amount == 0) {
                if (update_carry) {
                    arm_set_flag(cpu, FLAG_C, (val & 0x80000000u) != 0);
                }
                return val;
            } else {
                if (update_carry) {
                    arm_set_flag(cpu, FLAG_C, ((val >> (amount - 1)) & 1) != 0);
                }
                return (val >> amount) | (val << (32 - amount));
            }
        default:
            return val;
    }
}

static inline void set_nz_flags(ARM7TDMI *cpu, uint32_t result) {
    arm_set_flag(cpu, FLAG_N, (result & 0x80000000u) != 0);
    arm_set_flag(cpu, FLAG_Z, result == 0);
}

static inline uint32_t alu_add(ARM7TDMI *cpu, uint32_t a, uint32_t b, bool carry_in, bool set_flags) {
    uint64_t res = (uint64_t)a + (uint64_t)b + (carry_in ? 1 : 0);
    uint32_t r = (uint32_t)res;
    if (set_flags) {
        set_nz_flags(cpu, r);
        arm_set_flag(cpu, FLAG_C, res > 0xFFFFFFFFull);
        arm_set_flag(cpu, FLAG_V, (~(a ^ b) & (a ^ r) & 0x80000000u) != 0);
    }
    return r;
}

static inline uint32_t alu_sub(ARM7TDMI *cpu, uint32_t a, uint32_t b, bool carry_in, bool set_flags) {
    uint64_t res = (uint64_t)a - (uint64_t)b - (carry_in ? 0 : 1);
    uint32_t r = (uint32_t)res;
    if (set_flags) {
        set_nz_flags(cpu, r);
        arm_set_flag(cpu, FLAG_C, (uint64_t)a >= ((uint64_t)b + (carry_in ? 0 : 1)));
        arm_set_flag(cpu, FLAG_V, ((a ^ b) & (a ^ r) & 0x80000000u) != 0);
    }
    return r;
}

int arm_execute_instruction(ARM7TDMI *cpu, uint32_t opcode) {
    uint32_t cond = opcode >> 28;
    if (!arm7tdmi_check_condition(cpu, cond)) {
        return 1; // 1 cycle for skipped instruction
    }

    GBABus *bus = cpu->bus;

    // 1. Branch / Branch with Link (B, BL)
    if ((opcode & 0x0E000000) == 0x0A000000) {
        bool link = (opcode & 0x01000000) != 0;
        int32_t offset = (int32_t)(opcode & 0x00FFFFFF);
        if (offset & 0x00800000) {
            offset |= 0xFF000000; // Sign extend 24-bit offset
        }
        offset <<= 2;

        if (link) {
            cpu->r[REG_LR] = cpu->execute_addr + 4;
        }

        cpu->r[REG_PC] = (uint32_t)((int32_t)cpu->r[REG_PC] + offset);
        arm7tdmi_fill_pipeline_arm(cpu, bus);
        cpu->branched = true;
        return 3;
    }

    // 2. Branch and Exchange (BX)
    if ((opcode & 0x0FFFFFF0) == 0x012FFF10) {
        int rm = opcode & 0x0F;
        uint32_t target = cpu->r[rm];

        if (target & 1) {
            // Switch to THUMB mode
            cpu->cpsr |= FLAG_T;
            cpu->r[REG_PC] = target & ~1u;
            arm7tdmi_fill_pipeline_thumb(cpu, bus);
        } else {
            // Stay in ARM mode
            cpu->cpsr &= ~FLAG_T;
            cpu->r[REG_PC] = target & ~3u;
            arm7tdmi_fill_pipeline_arm(cpu, bus);
        }
        cpu->branched = true;
        return 3;
    }

    // 3. Software Interrupt (SWI)
    if ((opcode & 0x0F000000) == 0x0F000000) {
        uint8_t swi_num = (opcode >> 16) & 0xFF;
        if (cpu->bus && cpu->bus->gba) {
            bios_call(cpu->bus->gba, swi_num);
        } else {
            arm7tdmi_trigger_exception(cpu, EX_SWI);
        }
        return 3;
    }

    // 4. Status Register Transfer (MRS / MSR)
    if ((opcode & 0x0FBF0FFF) == 0x010F0000) { // MRS
        bool use_spsr = (opcode & (1 << 22)) != 0;
        int rd = (opcode >> 12) & 0xF;
        cpu->r[rd] = use_spsr ? cpu->spsr : cpu->cpsr;
        return 1;
    }
    if ((opcode & 0x0DB0F000) == 0x0120F000) { // MSR register/immediate
        bool use_spsr = (opcode & (1 << 22)) != 0;
        bool is_imm = (opcode & (1 << 25)) != 0;
        uint32_t val;
        if (is_imm) {
            uint32_t imm = opcode & 0xFF;
            uint32_t rot = ((opcode >> 8) & 0xF) * 2;
            val = (imm >> rot) | (imm << (32 - rot));
        } else {
            val = cpu->r[opcode & 0xF];
        }

        uint32_t mask = 0;
        if (opcode & (1 << 16)) mask |= 0x000000FF; // Control field
        if (opcode & (1 << 17)) mask |= 0x0000FF00; // Extension field
        if (opcode & (1 << 18)) mask |= 0x00FF0000; // Status field
        if (opcode & (1 << 19)) mask |= 0xFF000000; // Flags field

        if (use_spsr) {
            cpu->spsr = (cpu->spsr & ~mask) | (val & mask);
            ARMBankIndex bank = arm7tdmi_mode_to_bank(arm7tdmi_get_mode(cpu));
            if (bank != BANK_USR) cpu->banks.spsr[bank] = cpu->spsr;
        } else {
            if ((cpu->cpsr & CPSR_MODE_MASK) == MODE_USR) {
                mask &= 0xFF000000; // User mode can only alter condition flags
            }
            uint32_t new_cpsr = (cpu->cpsr & ~mask) | (val & mask);
            if ((new_cpsr & CPSR_MODE_MASK) != (cpu->cpsr & CPSR_MODE_MASK)) {
                arm7tdmi_switch_mode(cpu, (ARMMode)(new_cpsr & CPSR_MODE_MASK));
            }
            cpu->cpsr = new_cpsr;
        }
        return 1;
    }

    // 5. Multiply / Multiply Long (MUL, MLA, UMULL, UMLAL, SMULL, SMLAL)
    if ((opcode & 0x0FC000F0) == 0x00000090) { // MUL, MLA
        bool accumulate = (opcode & (1 << 21)) != 0;
        bool set_flags = (opcode & (1 << 20)) != 0;
        int rd = (opcode >> 16) & 0xF;
        int rn = (opcode >> 12) & 0xF;
        int rs = (opcode >> 8) & 0xF;
        int rm = opcode & 0xF;

        uint32_t res = cpu->r[rm] * cpu->r[rs];
        if (accumulate) res += cpu->r[rn];
        cpu->r[rd] = res;

        if (set_flags) {
            set_nz_flags(cpu, res);
        }
        return accumulate ? 3 : 2;
    }
    if ((opcode & 0x0F8000F0) == 0x00800090) { // Multiply Long
        bool is_signed = (opcode & (1 << 22)) != 0;
        bool accumulate = (opcode & (1 << 21)) != 0;
        bool set_flags = (opcode & (1 << 20)) != 0;
        int rd_hi = (opcode >> 16) & 0xF;
        int rd_lo = (opcode >> 12) & 0xF;
        int rs = (opcode >> 8) & 0xF;
        int rm = opcode & 0xF;

        uint64_t res;
        if (is_signed) {
            int64_t a = (int32_t)cpu->r[rm];
            int64_t b = (int32_t)cpu->r[rs];
            int64_t mult = a * b;
            if (accumulate) {
                int64_t acc = ((uint64_t)cpu->r[rd_hi] << 32) | cpu->r[rd_lo];
                mult += acc;
            }
            res = (uint64_t)mult;
        } else {
            uint64_t a = cpu->r[rm];
            uint64_t b = cpu->r[rs];
            uint64_t mult = a * b;
            if (accumulate) {
                uint64_t acc = ((uint64_t)cpu->r[rd_hi] << 32) | cpu->r[rd_lo];
                mult += acc;
            }
            res = mult;
        }

        cpu->r[rd_lo] = (uint32_t)res;
        cpu->r[rd_hi] = (uint32_t)(res >> 32);

        if (set_flags) {
            arm_set_flag(cpu, FLAG_N, (res & 0x8000000000000000ull) != 0);
            arm_set_flag(cpu, FLAG_Z, res == 0);
        }
        return accumulate ? 4 : 3;
    }

    // 6. Single Data Swap (SWP, SWPB)
    if ((opcode & 0x0FB00FF0) == 0x01000090) {
        bool byte_swap = (opcode & (1 << 22)) != 0;
        int rn = (opcode >> 16) & 0xF;
        int rd = (opcode >> 12) & 0xF;
        int rm = opcode & 0xF;
        uint32_t addr = cpu->r[rn];

        if (byte_swap) {
            uint8_t temp = bus_read8(bus, addr);
            bus_write8(bus, addr, (uint8_t)cpu->r[rm]);
            cpu->r[rd] = temp;
        } else {
            uint32_t temp = bus_read32(bus, addr);
            bus_write32(bus, addr, cpu->r[rm]);
            cpu->r[rd] = temp;
        }
        return 4;
    }

    // 7. Halfword & Signed Data Transfer (LDRH, STRH, LDRSB, LDRSH)
    if ((opcode & 0x0E000090) == 0x00000090 && (opcode & 0x00000060) != 0) {
        bool pre = (opcode & (1 << 24)) != 0;
        bool up = (opcode & (1 << 23)) != 0;
        bool imm_offset = (opcode & (1 << 22)) != 0;
        bool writeback = (opcode & (1 << 21)) != 0;
        bool load = (opcode & (1 << 20)) != 0;
        int rn = (opcode >> 16) & 0xF;
        int rd = (opcode >> 12) & 0xF;
        int op = (opcode >> 5) & 3;

        uint32_t offset = imm_offset ? (((opcode >> 8) & 0xF) << 4) | (opcode & 0xF) : cpu->r[opcode & 0xF];
        uint32_t addr = cpu->r[rn];
        if (rn == REG_PC) addr = (addr & ~3u); // Word-aligned when base is PC

        uint32_t eff_addr = up ? (addr + offset) : (addr - offset);
        uint32_t access_addr = pre ? eff_addr : addr;

        if (load) {
            switch (op) {
                case 1: // LDRH - Unsigned Halfword
                    cpu->r[rd] = bus_read16(bus, access_addr);
                    break;
                case 2: // LDRSB - Signed Byte
                    cpu->r[rd] = (int32_t)(int8_t)bus_read8(bus, access_addr);
                    break;
                case 3: // LDRSH - Signed Halfword
                    if (access_addr & 1) {
                        // Unaligned LDRSH reads sign-extended byte
                        cpu->r[rd] = (int32_t)(int8_t)bus_read8(bus, access_addr);
                    } else {
                        cpu->r[rd] = (int32_t)(int16_t)bus_read16(bus, access_addr);
                    }
                    break;
            }
        } else {
            // STRH - Store Halfword
            bus_write16(bus, access_addr, (uint16_t)cpu->r[rd]);
        }

        if (!pre) {
            cpu->r[rn] = eff_addr;
        } else if (writeback) {
            cpu->r[rn] = eff_addr;
        }

        return 3;
    }

    // 8. Block Data Transfer (LDM, STM)
    if ((opcode & 0x0E000000) == 0x08000000) {
        bool pre = (opcode & (1 << 24)) != 0;
        bool up = (opcode & (1 << 23)) != 0;
        bool s_bit = (opcode & (1 << 22)) != 0;
        bool writeback = (opcode & (1 << 21)) != 0;
        bool load = (opcode & (1 << 20)) != 0;
        int rn = (opcode >> 16) & 0xF;
        uint16_t rlist = opcode & 0xFFFF;

        int count = 0;
        for (int i = 0; i < 16; i++) {
            if (rlist & (1 << i)) count++;
        }

        uint32_t start_addr = cpu->r[rn];
        uint32_t addr;

        if (up) {
            addr = pre ? (start_addr + 4) : start_addr;
        } else {
            addr = start_addr - (count * 4);
            if (!pre) addr += 4;
        }

        if (writeback && !(load && (rlist & (1 << rn)))) {
            cpu->r[rn] = up ? (start_addr + count * 4) : (start_addr - count * 4);
        }

        bool user_bank = s_bit && !(load && (rlist & (1 << REG_PC)));

        for (int i = 0; i < 16; i++) {
            if (rlist & (1 << i)) {
                if (load) {
                    uint32_t val = bus_read32(bus, addr);
                    if (user_bank) {
                        if (i == REG_SP) cpu->banks.sp[BANK_USR] = val;
                        else if (i == REG_LR) cpu->banks.lr[BANK_USR] = val;
                        else cpu->r[i] = val;
                    } else {
                        cpu->r[i] = val;
                    }
                } else {
                    uint32_t val = cpu->r[i];
                    if (user_bank) {
                        if (i == REG_SP) val = cpu->banks.sp[BANK_USR];
                        else if (i == REG_LR) val = cpu->banks.lr[BANK_USR];
                    }
                    bus_write32(bus, addr, val);
                }
                addr += 4;
            }
        }

        // CRITICAL: PC restoration with S-bit in LDM
        if (load && (rlist & (1 << REG_PC))) {
            if (s_bit) {
                // Restore CPSR from SPSR
                ARMMode current_mode = arm7tdmi_get_mode(cpu);
                ARMBankIndex bank = arm7tdmi_mode_to_bank(current_mode);
                if (bank != BANK_USR) {
                    uint32_t target_cpsr = cpu->banks.spsr[bank];
                    arm7tdmi_switch_mode(cpu, (ARMMode)(target_cpsr & CPSR_MODE_MASK));
                    cpu->cpsr = target_cpsr;
                }
            }

            // Inspect restored CPSR bit 5 (FLAG_T)
            if (cpu->cpsr & FLAG_T) {
                cpu->r[REG_PC] &= ~1u;
                arm7tdmi_fill_pipeline_thumb(cpu, bus);
            } else {
                cpu->r[REG_PC] &= ~3u;
                arm7tdmi_fill_pipeline_arm(cpu, bus);
            }
            cpu->branched = true;
        }

        return count + 1;
    }

    // 9. Single Data Transfer (LDR, STR)
    if ((opcode & 0x0C000000) == 0x04000000) {
        bool is_reg_offset = (opcode & (1 << 25)) != 0;
        bool pre = (opcode & (1 << 24)) != 0;
        bool up = (opcode & (1 << 23)) != 0;
        bool byte_trans = (opcode & (1 << 22)) != 0;
        bool writeback = (opcode & (1 << 21)) != 0;
        bool load = (opcode & (1 << 20)) != 0;
        int rn = (opcode >> 16) & 0xF;
        int rd = (opcode >> 12) & 0xF;

        uint32_t offset;
        if (is_reg_offset) {
            int rm = opcode & 0xF;
            uint32_t shift_type = (opcode >> 5) & 3;
            uint32_t shift_imm = (opcode >> 7) & 0x1F;
            offset = arm_shift(cpu, cpu->r[rm], shift_type, shift_imm, false);
        } else {
            offset = opcode & 0xFFF;
        }

        uint32_t base = cpu->r[rn];
        if (rn == REG_PC) base = (base & ~3u); // Word-aligned base for PC

        uint32_t eff_addr = up ? (base + offset) : (base - offset);
        uint32_t access_addr = pre ? eff_addr : base;

        if (load) {
            uint32_t data;
            if (byte_trans) {
                data = bus_read8(bus, access_addr);
            } else {
                data = bus_read32(bus, access_addr);
            }
            cpu->r[rd] = data;

            if (rd == REG_PC) {
                cpu->r[REG_PC] &= ~3u;
                arm7tdmi_fill_pipeline_arm(cpu, bus);
                cpu->branched = true;
            }
        } else {
            uint32_t data = cpu->r[rd];
            if (rd == REG_PC) data += 4; // R15 store offset rule
            if (byte_trans) {
                bus_write8(bus, access_addr, (uint8_t)data);
            } else {
                bus_write32(bus, access_addr, data);
            }
        }

        if (!pre) {
            cpu->r[rn] = eff_addr;
        } else if (writeback) {
            cpu->r[rn] = eff_addr;
        }

        return load ? 3 : 2;
    }

    // 10. Data Processing ALU Operations
    if ((opcode & 0x0C000000) == 0x00000000) {
        bool is_imm = (opcode & (1 << 25)) != 0;
        uint32_t alu_op = (opcode >> 21) & 0xF;
        bool set_flags = (opcode & (1 << 20)) != 0;
        int rn = (opcode >> 16) & 0xF;
        int rd = (opcode >> 12) & 0xF;

        uint32_t op2;
        bool shifter_carry = arm_get_flag(cpu, FLAG_C);

        if (is_imm) {
            uint32_t imm = opcode & 0xFF;
            uint32_t rot = ((opcode >> 8) & 0xF) * 2;
            if (rot == 0) {
                op2 = imm;
            } else {
                op2 = (imm >> rot) | (imm << (32 - rot));
                shifter_carry = ((op2 >> 31) & 1) != 0;
            }
        } else {
            int rm = opcode & 0xF;
            uint32_t shift_type = (opcode >> 5) & 3;
            bool reg_shift = (opcode & (1 << 4)) != 0;
            uint32_t shift_amount;
            if (reg_shift) {
                int rs = (opcode >> 8) & 0xF;
                shift_amount = cpu->r[rs] & 0xFF;
            } else {
                shift_amount = (opcode >> 7) & 0x1F;
            }
            op2 = arm_shift(cpu, cpu->r[rm], shift_type, shift_amount, set_flags && (alu_op == 0x0 || alu_op == 0x1 || alu_op == 0x8 || alu_op == 0x9 || alu_op == 0xC || alu_op == 0xD || alu_op == 0xE || alu_op == 0xF));
            if (set_flags) shifter_carry = arm_get_flag(cpu, FLAG_C);
        }

        uint32_t op1 = cpu->r[rn];
        uint32_t result = 0;
        bool write_rd = true;

        switch (alu_op) {
            case 0x0: // AND
                result = op1 & op2;
                if (set_flags) { set_nz_flags(cpu, result); arm_set_flag(cpu, FLAG_C, shifter_carry); }
                break;
            case 0x1: // EOR
                result = op1 ^ op2;
                if (set_flags) { set_nz_flags(cpu, result); arm_set_flag(cpu, FLAG_C, shifter_carry); }
                break;
            case 0x2: // SUB
                result = alu_sub(cpu, op1, op2, true, set_flags);
                break;
            case 0x3: // RSB
                result = alu_sub(cpu, op2, op1, true, set_flags);
                break;
            case 0x4: // ADD
                result = alu_add(cpu, op1, op2, false, set_flags);
                break;
            case 0x5: // ADC
                result = alu_add(cpu, op1, op2, arm_get_flag(cpu, FLAG_C), set_flags);
                break;
            case 0x6: // SBC
                result = alu_sub(cpu, op1, op2, arm_get_flag(cpu, FLAG_C), set_flags);
                break;
            case 0x7: // RSC
                result = alu_sub(cpu, op2, op1, arm_get_flag(cpu, FLAG_C), set_flags);
                break;
            case 0x8: // TST
                set_nz_flags(cpu, op1 & op2);
                arm_set_flag(cpu, FLAG_C, shifter_carry);
                write_rd = false;
                break;
            case 0x9: // TEQ
                set_nz_flags(cpu, op1 ^ op2);
                arm_set_flag(cpu, FLAG_C, shifter_carry);
                write_rd = false;
                break;
            case 0xA: // CMP
                alu_sub(cpu, op1, op2, true, true);
                write_rd = false;
                break;
            case 0xB: // CMN
                alu_add(cpu, op1, op2, false, true);
                write_rd = false;
                break;
            case 0xC: // ORR
                result = op1 | op2;
                if (set_flags) { set_nz_flags(cpu, result); arm_set_flag(cpu, FLAG_C, shifter_carry); }
                break;
            case 0xD: // MOV
                result = op2;
                if (set_flags) { set_nz_flags(cpu, result); arm_set_flag(cpu, FLAG_C, shifter_carry); }
                break;
            case 0xE: // BIC
                result = op1 & ~op2;
                if (set_flags) { set_nz_flags(cpu, result); arm_set_flag(cpu, FLAG_C, shifter_carry); }
                break;
            case 0xF: // MVN
                result = ~op2;
                if (set_flags) { set_nz_flags(cpu, result); arm_set_flag(cpu, FLAG_C, shifter_carry); }
                break;
        }

        if (write_rd) {
            cpu->r[rd] = result;
        }

        // CRITICAL: IRQ Return Pipeline Refill (e.g. SUBS PC, LR, #4)
        if (rd == REG_PC) {
            if (set_flags) {
                // Restore CPSR from SPSR
                ARMMode current_mode = arm7tdmi_get_mode(cpu);
                ARMBankIndex bank = arm7tdmi_mode_to_bank(current_mode);
                if (bank != BANK_USR) {
                    uint32_t target_cpsr = cpu->banks.spsr[bank];
                    arm7tdmi_switch_mode(cpu, (ARMMode)(target_cpsr & CPSR_MODE_MASK));
                    cpu->cpsr = target_cpsr;
                }
            }

            // Inspect restored CPSR bit 5 (FLAG_T)
            if (cpu->cpsr & FLAG_T) {
                cpu->r[REG_PC] &= ~1u;
                arm7tdmi_fill_pipeline_thumb(cpu, bus);
            } else {
                cpu->r[REG_PC] &= ~3u;
                arm7tdmi_fill_pipeline_arm(cpu, bus);
            }
            cpu->branched = true;
            return 3;
        }

        return 1;
    }

    return 1;
}
