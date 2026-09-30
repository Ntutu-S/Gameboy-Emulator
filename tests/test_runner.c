#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "gba.h"
#include "arm7tdmi.h"
#include "bus.h"
#include "dma.h"
#include "timer.h"
#include "interrupt.h"
#include "ppu.h"
#include "bios.h"

static int g_tests_passed = 0;
static int g_tests_total = 18;

#define TEST_ASSERT(cond, name) do { \
    if (!(cond)) { \
        printf("  [FAIL] Test %s (Line %d)\n", name, __LINE__); \
        return false; \
    } \
} while (0)

#define RUN_TEST(fn, name) do { \
    printf("[%02d/18] Running: %s ... ", g_tests_passed + 1, name); \
    if (fn()) { \
        printf("PASSED\n"); \
        g_tests_passed++; \
    } else { \
        printf("FAILED\n"); \
    } \
} while (0)

// ----------------------------------------------------------------------------
// Test 1: EWRAM memory mirroring at 0x02041000
// ----------------------------------------------------------------------------
static bool test_01_ewram_mirroring(void) {
    GBA gba;
    gba_init(&gba);

    bus_write32(&gba.bus, 0x02001000, 0xCAFEBABE);
    uint32_t val = bus_read32(&gba.bus, 0x02041000); // Mirrored offset (0x02001000 + 256KB)
    TEST_ASSERT(val == 0xCAFEBABE, "EWRAM 256KB Mirroring");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 2: IWRAM memory mirroring at 0x03008500
// ----------------------------------------------------------------------------
static bool test_02_iwram_mirroring(void) {
    GBA gba;
    gba_init(&gba);

    bus_write16(&gba.bus, 0x03000500, 0x1234);
    uint16_t val = bus_read16(&gba.bus, 0x03008500); // Mirrored offset (0x03000500 + 32KB)
    TEST_ASSERT(val == 0x1234, "IWRAM 32KB Mirroring");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 3: Palette 8-bit write halfword expansion
// ----------------------------------------------------------------------------
static bool test_03_palette_byte_expansion(void) {
    GBA gba;
    gba_init(&gba);

    bus_write8(&gba.bus, 0x05000000, 0x5A);
    uint16_t hw = bus_read16(&gba.bus, 0x05000000);
    // Byte 0x5A expands to 0x5A5A
    TEST_ASSERT(hw == 0x5A5A, "Palette 8-bit write expansion");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 4: Aligned and unaligned 32-bit bus reads
// ----------------------------------------------------------------------------
static bool test_04_unaligned_bus_reads(void) {
    GBA gba;
    gba_init(&gba);

    bus_write32(&gba.bus, 0x02000000, 0x12345678);

    uint32_t r0 = bus_read32(&gba.bus, 0x02000000);
    TEST_ASSERT(r0 == 0x12345678, "Aligned 32-bit read");

    uint32_t r1 = bus_read32(&gba.bus, 0x02000001); // Rotate right by 8 bits
    TEST_ASSERT(r1 == 0x78123456, "Unaligned 32-bit read offset 1");

    uint32_t r2 = bus_read32(&gba.bus, 0x02000002); // Rotate right by 16 bits
    TEST_ASSERT(r2 == 0x56781234, "Unaligned 32-bit read offset 2");

    uint32_t r3 = bus_read32(&gba.bus, 0x02000003); // Rotate right by 24 bits
    TEST_ASSERT(r3 == 0x34567812, "Unaligned 32-bit read offset 3");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 5: DMA0 32-bit block transfers
// ----------------------------------------------------------------------------
static bool test_05_dma0_block_transfer(void) {
    GBA gba;
    gba_init(&gba);

    // Prepare source buffer in IWRAM
    for (int i = 0; i < 8; i++) {
        bus_write32(&gba.bus, 0x03000000 + i * 4, 0x1000 + i);
    }

    // Configure DMA0
    dma_write_src(&gba.dma, 0, 0x03000000, 0);
    dma_write_src(&gba.dma, 0, 0x03000000, 2);
    dma_write_dst(&gba.dma, 0, 0x02000000, 0);
    dma_write_dst(&gba.dma, 0, 0x02000000, 2);
    dma_write_count(&gba.dma, 0, 8);

    // Start immediate 32-bit transfer (Enable bit 15 + 32-bit bit 10)
    dma_write_cnt_h(&gba.dma, 0, (1 << 15) | (1 << 10));

    // Verify destination in EWRAM
    for (int i = 0; i < 8; i++) {
        uint32_t val = bus_read32(&gba.bus, 0x02000000 + i * 4);
        TEST_ASSERT(val == 0x1000u + i, "DMA0 block transfer data match");
    }

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 6: EEPROM 64-bit block writes via DMA3
// ----------------------------------------------------------------------------
static bool test_06_eeprom_block_write(void) {
    GBA gba;
    gba_init(&gba);

    // Setup EEPROM write stream:
    // Command '10' (write) + 14-bit address (e.g. 0x0001) + 64 bits data + 0 stop
    uint16_t stream[81];
    int pos = 0;
    stream[pos++] = 1; stream[pos++] = 0; // Command 10 (write)
    // 14-bit address = 1 (00 0000 0000 0001)
    for (int i = 13; i >= 0; i--) {
        stream[pos++] = (1 >> i) & 1;
    }
    // 64 bits data: 8 bytes of 0xAA (10101010)
    for (int i = 0; i < 64; i++) {
        stream[pos++] = (i % 2 == 0) ? 1 : 0;
    }
    stream[pos++] = 0; // Stop bit

    for (int i = 0; i < pos; i++) {
        bus_write16(&gba.bus, 0x03000000 + i * 2, stream[i]);
    }

    // Run DMA3 to write to EEPROM
    dma_write_src(&gba.dma, 3, 0x03000000, 0);
    dma_write_src(&gba.dma, 3, 0x03000000, 2);
    dma_write_dst(&gba.dma, 3, 0x0D000000, 0);
    dma_write_dst(&gba.dma, 3, 0x0D000000, 2);
    dma_write_count(&gba.dma, 3, pos);
    dma_write_cnt_h(&gba.dma, 3, (1 << 15)); // 16-bit immediate transfer

    // Check EEPROM buffer for written data at block 1 (byte offset 8)
    TEST_ASSERT(gba.bus.eeprom.data[8] == 0xAA, "EEPROM block byte 0 written");
    TEST_ASSERT(gba.bus.eeprom.data[9] == 0xAA, "EEPROM block byte 1 written");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 7: EEPROM 68-bit serial read stream via DMA3
// ----------------------------------------------------------------------------
static bool test_07_eeprom_serial_read(void) {
    GBA gba;
    gba_init(&gba);

    // Prepopulate EEPROM page 2 (bytes 16..23) with known data
    for (int i = 0; i < 8; i++) {
        gba.bus.eeprom.data[16 + i] = 0x55; // 01010101
    }

    // Step 1: Send read request command (11) + 14-bit address 2 + stop 0
    uint16_t req[17];
    int pos = 0;
    req[pos++] = 1; req[pos++] = 1; // Command 11 (read)
    for (int i = 13; i >= 0; i--) {
        req[pos++] = (2 >> i) & 1; // Address 2
    }
    req[pos++] = 0;

    for (int i = 0; i < pos; i++) {
        bus_write16(&gba.bus, 0x03000000 + i * 2, req[i]);
    }

    dma_write_src(&gba.dma, 3, 0x03000000, 0);
    dma_write_src(&gba.dma, 3, 0x03000000, 2);
    dma_write_dst(&gba.dma, 3, 0x0D000000, 0);
    dma_write_dst(&gba.dma, 3, 0x0D000000, 2);
    dma_write_count(&gba.dma, 3, pos);
    dma_write_cnt_h(&gba.dma, 3, (1 << 15));

    // Step 2: Read 68-halfword response stream via DMA3
    dma_write_src(&gba.dma, 3, 0x0D000000, 0);
    dma_write_src(&gba.dma, 3, 0x0D000000, 2);
    dma_write_dst(&gba.dma, 3, 0x03000100, 0);
    dma_write_dst(&gba.dma, 3, 0x03000100, 2);
    dma_write_count(&gba.dma, 3, 68);
    dma_write_cnt_h(&gba.dma, 3, (1 << 15));

    // Verify 4 dummy bits are 0
    for (int i = 0; i < 4; i++) {
        uint16_t bit = bus_read16(&gba.bus, 0x03000100 + i * 2);
        TEST_ASSERT(bit == 0, "EEPROM read dummy bit is 0");
    }

    // Verify first data bit of 0x55 (msb is 0)
    uint16_t b0 = bus_read16(&gba.bus, 0x03000100 + 4 * 2);
    uint16_t b1 = bus_read16(&gba.bus, 0x03000100 + 5 * 2);
    TEST_ASSERT(b0 == 0, "EEPROM bit 0 of 0x55");
    TEST_ASSERT(b1 == 1, "EEPROM bit 1 of 0x55");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 8: Timer 0 overflow and IRQ flag setting
// ----------------------------------------------------------------------------
static bool test_08_timer0_overflow_irq(void) {
    GBA gba;
    gba_init(&gba);

    // Reload value 0xFFFE
    timer_write_reload(&gba.timer, 0, 0xFFFE);
    // Control: Enable (bit 7) + IRQ enable (bit 6) + Prescaler /1 (shift 0)
    timer_write_control(&gba.timer, 0, (1 << 7) | (1 << 6) | 0);

    TEST_ASSERT((gba.intr.if_ & INTR_TIMER0) == 0, "Timer 0 IF clear before overflow");

    // Step 2 cycles (counter 0xFFFE -> 0xFFFF -> overflow)
    timer_step(&gba.timer, 2);

    TEST_ASSERT((gba.intr.if_ & INTR_TIMER0) != 0, "Timer 0 overflow set IF flag");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 9: BIOS IRQ vector trampoline and user handler dispatch
// ----------------------------------------------------------------------------
static bool test_09_bios_irq_trampoline(void) {
    GBA gba;
    gba_init(&gba);
    bios_install_trampoline(&gba);

    // Set custom handler at 0x03007FFC
    bus_write32(&gba.bus, 0x03007FFC, 0x03000040);
    // Place BX LR at 0x03000040: 0xE12FFF1E
    bus_write32(&gba.bus, 0x03000040, 0xE12FFF1E);

    // Raise VBlank interrupt
    gba.intr.ime = 1;
    gba.intr.ie = INTR_VBLANK;
    interrupt_raise(&gba.intr, INTR_VBLANK);

    // Trigger IRQ exception
    arm7tdmi_trigger_exception(&gba.cpu, EX_IRQ);

    // Verify CPU entered IRQ mode, jumped to 0x18
    TEST_ASSERT(arm7tdmi_get_mode(&gba.cpu) == MODE_IRQ, "CPU entered IRQ mode");
    TEST_ASSERT(gba.cpu.execute_addr == 0x00000018, "CPU at IRQ vector 0x18");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 10: ARM arithmetic instructions with synthetic ROM
// ----------------------------------------------------------------------------
static bool test_10_arm_arithmetic(void) {
    GBA gba;
    gba_init(&gba);
    if (!gba_load_rom(&gba, "tests/arm_test.gba")) return false;
    gba_reset(&gba, true); // Skip intro to start at 0x08000000

    // Step through initial branch and arithmetic instructions
    for (int i = 0; i < 6; i++) {
        arm7tdmi_step(&gba.cpu);
    }

    // Verify R0 = 125, R1 = 75, and Z flag set from CMP
    TEST_ASSERT(gba.cpu.r[0] == 125, "ARM ADD calculation R0=125");
    TEST_ASSERT(gba.cpu.r[1] == 75, "ARM SUB calculation R1=75");
    TEST_ASSERT(arm_get_flag(&gba.cpu, FLAG_Z) == true, "ARM CMP set Z flag");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 11: ARM barrel shifter operations with synthetic ROM
// ----------------------------------------------------------------------------
static bool test_11_arm_barrel_shifter(void) {
    GBA gba;
    gba_init(&gba);
    if (!gba_load_rom(&gba, "tests/arm_test.gba")) return false;
    gba_reset(&gba, true);

    // Step through to shifter instructions (instructions 6 to 11)
    for (int i = 0; i < 11; i++) {
        arm7tdmi_step(&gba.cpu);
    }

    TEST_ASSERT(gba.cpu.r[3] == 16, "ARM LSL #4 (1 << 4 = 16)");
    TEST_ASSERT(gba.cpu.r[4] == 4,  "ARM LSR #2 (16 >> 2 = 4)");
    TEST_ASSERT(gba.cpu.r[5] == 2,  "ARM ASR #1 (4 >> 1 = 2)");
    TEST_ASSERT(gba.cpu.r[6] == 1,  "ARM ROR #1 (2 ROR 1 = 1)");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 12: ARM branch and exchange (B, BL, BX) with synthetic ROM
// ----------------------------------------------------------------------------
static bool test_12_arm_branch_and_exchange(void) {
    GBA gba;
    gba_init(&gba);
    if (!gba_load_rom(&gba, "tests/arm_test.gba")) return false;
    gba_reset(&gba, true);

    // Step until after BL call and return from target
    for (int i = 0; i < 16; i++) {
        arm7tdmi_step(&gba.cpu);
    }

    // Target subroutine sets R7 = R0 + 1 = 126
    TEST_ASSERT(gba.cpu.r[7] == 126, "ARM BL called target and executed R7=126");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 13: THUMB push/pop and stack operations with synthetic ROM
// ----------------------------------------------------------------------------
static bool test_13_thumb_push_pop(void) {
    GBA gba;
    gba_init(&gba);
    if (!gba_load_rom(&gba, "tests/thumb_test.gba")) return false;
    gba_reset(&gba, true);

    // Step through ARM prelude to THUMB switch and stack push/pop
    for (int i = 0; i < 10; i++) {
        arm7tdmi_step(&gba.cpu);
    }

    TEST_ASSERT(arm_is_thumb(&gba.cpu) == true, "CPU switched to THUMB mode");
    TEST_ASSERT(gba.cpu.r[4] == 50, "THUMB POP R4=50 from stack");
    TEST_ASSERT(gba.cpu.r[6] == 70, "THUMB POP R6=70 from stack");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 14: THUMB arithmetic & logic with synthetic ROM
// ----------------------------------------------------------------------------
static bool test_14_thumb_arithmetic(void) {
    GBA gba;
    gba_init(&gba);
    if (!gba_load_rom(&gba, "tests/thumb_test.gba")) return false;
    gba_reset(&gba, true);

    // Step through initial THUMB math instructions
    for (int i = 0; i < 7; i++) {
        arm7tdmi_step(&gba.cpu);
    }

    TEST_ASSERT(gba.cpu.r[0] == 50, "THUMB ADD imm R0=50");
    TEST_ASSERT(gba.cpu.r[1] == 20, "THUMB MOV imm R1=20");
    TEST_ASSERT(gba.cpu.r[2] == 70, "THUMB ADD reg R2=70");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 15: THUMB branches and conditional branches with synthetic ROM
// ----------------------------------------------------------------------------
static bool test_15_thumb_branches(void) {
    GBA gba;
    gba_init(&gba);
    if (!gba_load_rom(&gba, "tests/thumb_test.gba")) return false;
    gba_reset(&gba, true);

    // Step past unconditional branch
    for (int i = 0; i < 12; i++) {
        arm7tdmi_step(&gba.cpu);
    }

    TEST_ASSERT(gba.cpu.r[7] == 1, "THUMB branch took and executed target");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 16: PPU Mode 3 bitmap rasterization
// ----------------------------------------------------------------------------
static bool test_16_ppu_mode3_bitmap(void) {
    GBA gba;
    gba_init(&gba);

    // Set Mode 3 (bit 0..2 = 3, bit 10 = enable BG2)
    gba.ppu.dispcnt = 3 | (1 << 10);

    // Write red pixel (RGB555 0x001F) to VRAM (0, 0)
    bus_write16(&gba.bus, 0x06000000, 0x001F);

    // Render scanline 0
    gba.ppu.vcount = 0;
    ppu_render_scanline(&gba.ppu);

    // Check ARGB8888 framebuffer at (0, 0) -> pure red (0xFFFF0000)
    uint32_t pixel = gba.ppu.framebuffer[0];
    TEST_ASSERT((pixel & 0x00FF0000) == 0x00FF0000, "Mode 3 Red channel match");
    TEST_ASSERT((pixel & 0x0000FF00) == 0, "Mode 3 Green channel clear");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 17: PPU Mode 0 / Tile Background rendering
// ----------------------------------------------------------------------------
static bool test_17_ppu_mode0_tiles(void) {
    GBA gba;
    gba_init(&gba);

    // Mode 0, enable BG0
    gba.ppu.dispcnt = 0 | (1 << 8);
    gba.ppu.bgcnt[0] = (1 << 8); // Screen base block 1 (0x06000800), Char base 0 (0x06000000), 16-color

    // Set Palette index 1 = Green (0x03E0)
    bus_write16(&gba.bus, 0x05000002, 0x03E0);

    // Tile 0 pixel (0,0) = color 1
    bus_write16(&gba.bus, 0x06000000, 0x0001);
    // Screen map (0,0) = tile 0 at screen base block 1
    bus_write16(&gba.bus, 0x06000800, 0x0000);

    gba.ppu.vcount = 0;
    ppu_render_scanline(&gba.ppu);

    uint32_t pixel = gba.ppu.framebuffer[0];
    TEST_ASSERT((pixel & 0x0000FF00) == 0x0000FF00, "Mode 0 Tile Green pixel match");

    gba_cleanup(&gba);
    return true;
}

// ----------------------------------------------------------------------------
// Test 18: PPU Sprite / OBJ priority precedence over Background
// ----------------------------------------------------------------------------
static bool test_18_ppu_obj_priority_precedence(void) {
    GBA gba;
    gba_init(&gba);

    // Enable BG0 and OBJ
    gba.ppu.dispcnt = 0 | (1 << 8) | (1 << 12);
    // BG0 at priority 1, screen base block 1
    gba.ppu.bgcnt[0] = 1 | (1 << 8);

    // BG Palette 1 = Blue (0x7C00)
    bus_write16(&gba.bus, 0x05000002, 0x7C00);
    // Tile 0 pixel (0,0) = color 1
    bus_write16(&gba.bus, 0x06000000, 0x0001);
    // Screen map (0,0) = tile 0 at screen base block 1
    bus_write16(&gba.bus, 0x06000800, 0x0000);

    // OBJ Palette 1 = Red (0x001F)
    bus_write16(&gba.bus, 0x05000202, 0x001F);
    // Sprite 0 at (0, 0), Priority 1 (same as BG0!)
    // Attr 0: Y=0
    bus_write16(&gba.bus, 0x07000000, 0x0000);
    // Attr 1: X=0
    bus_write16(&gba.bus, 0x07000002, 0x0000);
    // Attr 2: Tile 0, Priority 1 (1 << 10)
    bus_write16(&gba.bus, 0x07000004, (1 << 10));

    // Sprite Tile 0 pixel (0,0) = color 1
    // Write 16-bit halfword to OBJ VRAM 0x06010000 (8-bit writes are ignored by hardware)
    bus_write16(&gba.bus, 0x06010000, 0x0001);

    gba.ppu.vcount = 0;
    ppu_render_scanline(&gba.ppu);

    // CRITICAL Compositor Priority Precedence Rule:
    // When OBJ and BG share the same priority (1), OBJ takes precedence!
    // Therefore, pixel (0,0) MUST be OBJ's Red color, NOT BG0's Blue color!
    uint32_t pixel = gba.ppu.framebuffer[0];
    TEST_ASSERT((pixel & 0x00FF0000) == 0x00FF0000, "OBJ takes precedence over BG0 at equal priority");
    TEST_ASSERT((pixel & 0x000000FF) == 0, "BG0 Blue color occluded by OBJ");

    gba_cleanup(&gba);
    return true;
}

int main(void) {
    printf("=====================================================\n");
    printf("   NTUTU ADVANCE SYSTEM - HARDWARE TEST RUNNER       \n");
    printf("     Verifying 18 Core Hardware Subsystems           \n");
    printf("=====================================================\n\n");

    RUN_TEST(test_01_ewram_mirroring, "EWRAM 256KB Mirroring");
    RUN_TEST(test_02_iwram_mirroring, "IWRAM 32KB Mirroring");
    RUN_TEST(test_03_palette_byte_expansion, "Palette 8-bit Write Expansion");
    RUN_TEST(test_04_unaligned_bus_reads, "Aligned & Unaligned 32-bit Bus Reads");
    RUN_TEST(test_05_dma0_block_transfer, "DMA0 32-bit Block Transfers");
    RUN_TEST(test_06_eeprom_block_write, "EEPROM 64-bit Block Writes via DMA3");
    RUN_TEST(test_07_eeprom_serial_read, "EEPROM 68-bit Serial Read Stream via DMA3");
    RUN_TEST(test_08_timer0_overflow_irq, "Timer 0 Overflow & IRQ Flag Setting");
    RUN_TEST(test_09_bios_irq_trampoline, "BIOS IRQ Trampoline & Dispatch");
    RUN_TEST(test_10_arm_arithmetic, "ARM Arithmetic Instructions (ADD/SUB/CMP)");
    RUN_TEST(test_11_arm_barrel_shifter, "ARM Barrel Shifter (LSL/LSR/ASR/ROR)");
    RUN_TEST(test_12_arm_branch_and_exchange, "ARM Branch & Exchange (B/BL/BX)");
    RUN_TEST(test_13_thumb_push_pop, "THUMB PUSH/POP & Stack Operations");
    RUN_TEST(test_14_thumb_arithmetic, "THUMB Arithmetic & Logic");
    RUN_TEST(test_15_thumb_branches, "THUMB Branches & Control Flow");
    RUN_TEST(test_16_ppu_mode3_bitmap, "PPU Mode 3 Bitmap Rasterization");
    RUN_TEST(test_17_ppu_mode0_tiles, "PPU Mode 0 Tile Background Rendering");
    RUN_TEST(test_18_ppu_obj_priority_precedence, "PPU Sprite/OBJ Priority Precedence");

    printf("\n=====================================================\n");
    printf("   TEST RESULTS: %d / %d PASSED\n", g_tests_passed, g_tests_total);
    printf("=====================================================\n");

    return (g_tests_passed == g_tests_total) ? 0 : 1;
}
