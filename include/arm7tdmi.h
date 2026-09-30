#ifndef ARM7TDMI_H
#define ARM7TDMI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct GBABus;

/**
 * ARM7TDMI (ARMv4T Architecture) 32-bit RISC Processor Subsystem
 * "Ntutu Advance System"
 */

/* =========================================================================
 * 1. Operating Modes (ARMv4T)
 * ========================================================================= */
typedef enum ARMMode {
    MODE_USR = 0x10, // User mode (normal execution, unprivileged)
    MODE_FIQ = 0x11, // Fast Interrupt Request mode (banked R8-R14, SPSR)
    MODE_IRQ = 0x12, // Interrupt Request mode (banked R13-R14, SPSR)
    MODE_SVC = 0x13, // Supervisor mode (SWI/Reset entry, banked R13-R14, SPSR)
    MODE_ABT = 0x17, // Abort mode (banked R13-R14, SPSR)
    MODE_UND = 0x1B, // Undefined instruction mode (banked R13-R14, SPSR)
    MODE_SYS = 0x1F  // System mode (privileged User mode, shares User registers)
} ARMMode;

typedef enum ARMBankIndex {
    BANK_USR = 0,
    BANK_FIQ = 1,
    BANK_IRQ = 2,
    BANK_SVC = 3,
    BANK_ABT = 4,
    BANK_UND = 5,
    BANK_COUNT = 6
} ARMBankIndex;

/* =========================================================================
 * 2. CPSR / SPSR Status Flags Bitmasks
 * ========================================================================= */
#define FLAG_N         (1u << 31) // Bit 31: Negative / Less than condition flag
#define FLAG_Z         (1u << 30) // Bit 30: Zero condition flag
#define FLAG_C         (1u << 29) // Bit 29: Carry / Borrow / Extend flag
#define FLAG_V         (1u << 28) // Bit 28: Overflow condition flag

#define FLAG_I         (1u << 7)  // Bit 7:  IRQ disable (1 = IRQ disabled)
#define FLAG_F         (1u << 6)  // Bit 6:  FIQ disable (1 = FIQ disabled)
#define FLAG_T         (1u << 5)  // Bit 5:  State bit (0 = ARM 32-bit, 1 = THUMB 16-bit)
#define CPSR_MODE_MASK 0x1F       // Bits 4-0: Operating Mode bits M[4:0]

// Compatibility aliases
#define CPSR_N FLAG_N
#define CPSR_Z FLAG_Z
#define CPSR_C FLAG_C
#define CPSR_V FLAG_V
#define CPSR_I FLAG_I
#define CPSR_F FLAG_F
#define CPSR_T FLAG_T

/* =========================================================================
 * 3. Register Indices
 * ========================================================================= */
#define REG_SP 13 // R13: Stack Pointer
#define REG_LR 14 // R14: Link Register
#define REG_PC 15 // R15: Program Counter

/* =========================================================================
 * 4. Exception Vectors & Offsets
 * ========================================================================= */
typedef enum ARMException {
    EX_RESET          = 0x00000000,
    EX_UNDEFINED_INST = 0x00000004,
    EX_SWI            = 0x00000008,
    EX_PREFETCH_ABORT = 0x0000000C,
    EX_DATA_ABORT     = 0x00000010,
    EX_RESERVED       = 0x00000014,
    EX_IRQ            = 0x00000018,
    EX_FIQ            = 0x0000001C
} ARMException;

/* =========================================================================
 * 5. Banked Registers Storage
 * ========================================================================= */
typedef struct ARMBankedRegs {
    uint32_t sp[BANK_COUNT];      // Banked R13
    uint32_t lr[BANK_COUNT];      // Banked R14
    uint32_t spsr[BANK_COUNT];    // Banked SPSR
    uint32_t r8_12_fiq[5];        // Banked R8-R12 for FIQ
    uint32_t r8_12_usr[5];        // User R8-R12 storage while in FIQ
} ARMBankedRegs;

/* =========================================================================
 * 6. ARM7TDMI Processor State
 * ========================================================================= */
typedef struct ARM7TDMI {
    // General-purpose 32-bit registers (R0-R15)
    uint32_t r[16];

    // Status registers
    uint32_t cpsr; // Current Program Status Register
    uint32_t spsr; // Saved Program Status Register for active mode

    // Banked registers for 7 modes
    ARMBankedRegs banks;

    // 3-Stage instruction pipeline:
    // pipeline[0]: instruction currently in Decode stage (next to execute)
    // pipeline[1]: instruction currently in Fetch stage
    // When executing at A, PC = A + 8 (ARM) or A + 4 (THUMB)
    uint32_t pipeline[2];
    bool branched; // Set true when branch occurs to trigger pipeline reload
    uint32_t execute_addr; // Address of current executing instruction

    // Reference to memory bus
    struct GBABus *bus;

    // Cycle tracking & halt status
    uint64_t cycles;
    bool halted;
} ARM7TDMI;

/* =========================================================================
 * 7. Core Function Prototypes
 * ========================================================================= */
void arm7tdmi_init(ARM7TDMI *cpu, struct GBABus *bus);
void arm7tdmi_reset(ARM7TDMI *cpu);
void arm7tdmi_switch_mode(ARM7TDMI *cpu, ARMMode new_mode);
ARMMode arm7tdmi_get_mode(const ARM7TDMI *cpu);

void arm7tdmi_fill_pipeline_arm(ARM7TDMI *cpu, struct GBABus *bus);
void arm7tdmi_fill_pipeline_thumb(ARM7TDMI *cpu, struct GBABus *bus);

int arm7tdmi_step(ARM7TDMI *cpu);
void arm7tdmi_trigger_exception(ARM7TDMI *cpu, ARMException ex);

uint32_t arm7tdmi_get_reg(const ARM7TDMI *cpu, int reg);
void arm7tdmi_set_reg(ARM7TDMI *cpu, int reg, uint32_t val);

// Compatibility wrappers for existing code
static inline void arm_init(ARM7TDMI *cpu, struct GBABus *bus) { arm7tdmi_init(cpu, bus); }
static inline void arm_reset(ARM7TDMI *cpu) { arm7tdmi_reset(cpu); }
static inline void arm_set_mode(ARM7TDMI *cpu, ARMMode m) { arm7tdmi_switch_mode(cpu, m); }
static inline ARMMode arm_get_mode(const ARM7TDMI *cpu) { return arm7tdmi_get_mode(cpu); }
static inline uint32_t arm_get_reg(const ARM7TDMI *cpu, int reg) { return arm7tdmi_get_reg(cpu, reg); }
static inline void arm_set_reg(ARM7TDMI *cpu, int reg, uint32_t val) { arm7tdmi_set_reg(cpu, reg, val); }
static inline void arm_trigger_exception(ARM7TDMI *cpu, ARMException ex) { arm7tdmi_trigger_exception(cpu, ex); }

/* =========================================================================
 * 8. Status Register Helpers
 * ========================================================================= */
static inline bool arm_get_flag(const ARM7TDMI *cpu, uint32_t flag) {
    return (cpu->cpsr & flag) != 0;
}

static inline void arm_set_flag(ARM7TDMI *cpu, uint32_t flag, bool val) {
    if (val) {
        cpu->cpsr |= flag;
    } else {
        cpu->cpsr &= ~flag;
    }
}

static inline bool arm_is_thumb(const ARM7TDMI *cpu) {
    return (cpu->cpsr & FLAG_T) != 0;
}

static inline ARMBankIndex arm7tdmi_mode_to_bank(ARMMode mode) {
    switch (mode) {
        case MODE_FIQ: return BANK_FIQ;
        case MODE_IRQ: return BANK_IRQ;
        case MODE_SVC: return BANK_SVC;
        case MODE_ABT: return BANK_ABT;
        case MODE_UND: return BANK_UND;
        case MODE_USR:
        case MODE_SYS:
        default:
            return BANK_USR;
    }
}

bool arm7tdmi_check_condition(const ARM7TDMI *cpu, uint32_t cond);

#endif // ARM7TDMI_H
