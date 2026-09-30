#ifndef ARM_INSTRUCTIONS_H
#define ARM_INSTRUCTIONS_H

#include <stdint.h>
#include <stdbool.h>
#include "arm7tdmi.h"

int arm_execute_instruction(ARM7TDMI *cpu, uint32_t opcode);
uint32_t arm_shift(ARM7TDMI *cpu, uint32_t val, uint32_t shift_type, uint32_t amount, bool update_carry);

#endif // ARM_INSTRUCTIONS_H
