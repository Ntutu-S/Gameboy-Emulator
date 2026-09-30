#ifndef THUMB_INSTRUCTIONS_H
#define THUMB_INSTRUCTIONS_H

#include <stdint.h>
#include <stdbool.h>
#include "arm7tdmi.h"

int thumb_execute_instruction(ARM7TDMI *cpu, uint16_t opcode);

#endif // THUMB_INSTRUCTIONS_H
