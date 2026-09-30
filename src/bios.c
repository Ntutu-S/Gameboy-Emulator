#include "bios.h"
#include "gba.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/**
 * High-Level BIOS (HLE) & Ntutu Advance Branding Boot Sequence
 * "Ntutu Advance System"
 */

// Embedded 5x7 ASCII bitmap font (capital letters and basic symbols)
static const uint8_t font5x7[128][5] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['-'] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['A'] = {0x7C, 0x12, 0x11, 0x12, 0x7C},
    ['B'] = {0x7F, 0x49, 0x49, 0x49, 0x36},
    ['C'] = {0x3E, 0x41, 0x41, 0x41, 0x22},
    ['D'] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
    ['E'] = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ['M'] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    ['N'] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
    ['S'] = {0x46, 0x49, 0x49, 0x49, 0x31},
    ['T'] = {0x01, 0x01, 0x7F, 0x01, 0x01},
    ['U'] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ['V'] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
    ['Y'] = {0x07, 0x08, 0x70, 0x08, 0x07}
};

void bios_install_trampoline(GBA *gba) {
    if (!gba) return;
    uint32_t *bios_words = (uint32_t*)&gba->bus.bios[0x18];
    // Pre-install ARM trampoline at 0x00000018:
    // 0x18: stmdb sp!, {r0-r3, r12, lr}
    bios_words[0] = 0xE92D500F;
    // 0x1C: mov   r0, #0x04000000
    bios_words[1] = 0xE3A00404;
    // 0x20: ldr   r1, [r0, #-4] (loads from 0x03007FFC)
    bios_words[2] = 0xE5101004;
    // 0x24: cmp   r1, #0
    bios_words[3] = 0xE3510000;
    // 0x28: beq   +4
    bios_words[4] = 0x0A000001;
    // 0x2C: add   lr, pc, #0
    bios_words[5] = 0xE28FE000;
    // 0x30: bx    r1
    bios_words[6] = 0xE12FFF11;
    // 0x34: ldmia sp!, {r0-r3, r12, lr}
    bios_words[7] = 0xE8BD500F;
    // 0x38: subs  pc, lr, #4
    bios_words[8] = 0xE25EF004;
}

void bios_init(BIOS *bios, GBA *gba) {
    if (!bios) return;
    bios->gba = gba;
    bios->intro_active = !gba->skip_intro;
    bios->intro_frame = 0;
    bios->skip_intro = gba->skip_intro;
    bios_install_trampoline(gba);
}

void bios_finish_boot(GBA *gba) {
    if (!gba) return;
    ARM7TDMI *cpu = &gba->cpu;

    // Post-boot CPU register state:
    // SP_usr = 0x03007F00, SP_irq = 0x03007FA0, SP_svc = 0x03007FE0, CPSR = MODE_SYS, PC = 0x08000000
    cpu->banks.sp[BANK_USR] = 0x03007F00;
    cpu->banks.sp[BANK_IRQ] = 0x03007FA0;
    cpu->banks.sp[BANK_SVC] = 0x03007FE0;

    arm7tdmi_switch_mode(cpu, MODE_SYS);
    cpu->r[REG_SP] = 0x03007F00;
    cpu->r[REG_PC] = 0x08000000;

    arm7tdmi_fill_pipeline_arm(cpu, &gba->bus);
    gba->bios.intro_active = false;
}

static void draw_char(uint32_t *fb, int x, int y, char c, uint32_t color, int scale) {
    if ((unsigned char)c >= 128) return;
    const uint8_t *glyph = font5x7[(unsigned char)c];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        int px = x + col * scale + dx;
                        int py = y + row * scale + dy;
                        if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                            fb[py * SCREEN_WIDTH + px] = color;
                        }
                    }
                }
            }
        }
    }
}

static void draw_text(uint32_t *fb, int x, int y, const char *str, uint32_t color, int scale) {
    int cx = x;
    while (*str) {
        draw_char(fb, cx, y, *str, color, scale);
        cx += 6 * scale;
        str++;
    }
}

bool bios_render_intro_frame(BIOS *bios) {
    if (!bios || !bios->intro_active) return false;

    GBA *gba = bios->gba;
    uint32_t *fb = gba->ppu.framebuffer;
    int f = bios->intro_frame;

    // Check skip condition: Start (bit 3) or A (bit 0) pressed
    if (!(gba->keypad.keyinput & (1 << KEY_START)) || !(gba->keypad.keyinput & (1 << KEY_A))) {
        bios_finish_boot(gba);
        return false;
    }

    // 1. Dark space background with twinkling stars
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        fb[i] = 0xFF050510; // Dark navy space
    }
    // Pseudo-random stars based on frame
    for (int s = 0; s < 40; s++) {
        int sx = (s * 47 + f * 2) % SCREEN_WIDTH;
        int sy = (s * 31 + s * s) % SCREEN_HEIGHT;
        uint32_t star_c = ((s + f) % 5 == 0) ? 0xFFFFFFFF : 0xFF667799;
        fb[sy * SCREEN_WIDTH + sx] = star_c;
    }

    // 2. Animated Typography: "NTUTU ADVANCE SYSTEM"
    // Title 1: "NTUTU ADVANCE" in glowing gold
    uint32_t gold = 0xFFFFD700;
    uint32_t cyan = 0xFF00E5FF;
    if (f < 45) {
        gold = 0xFF886600 | ((f * 5) << 16) | ((f * 4) << 8);
    }

    draw_text(fb, 36, 55, "NTUTU ADVANCE", gold, 2);
    draw_text(fb, 75, 85, "SYSTEM", cyan, 2);

    // 3. Synthesize harmonic 2-tone boot chime through APU Channel 1 during frames 10-50
    if (f >= 10 && f < 30) {
        gba->apu.ch1.enabled = true;
        gba->apu.ch1.duty_length = (2 << 6) | (15 << 12); // 50% duty, max vol
        gba->apu.ch1.freq = 0x8000 | 1750; // Note 1 (~1046 Hz)
        gba->apu.soundcnt_l |= (1 << 8) | (1 << 12);
    } else if (f >= 30 && f < 50) {
        gba->apu.ch1.enabled = true;
        gba->apu.ch1.duty_length = (2 << 6) | (15 << 12);
        gba->apu.ch1.freq = 0x8000 | 1850; // Note 2 (~1318 Hz)
    } else if (f == 50) {
        gba->apu.ch1.enabled = false;
    }

    bios->intro_frame++;
    if (bios->intro_frame >= 90) {
        bios_finish_boot(gba);
        return false;
    }

    return true;
}

// ----------------------------------------------------------------------------
// Built-in HLE BIOS Routines
// ----------------------------------------------------------------------------
void bios_call(GBA *gba, uint8_t swi_num) {
    ARM7TDMI *cpu = &gba->cpu;
    GBABus *bus = &gba->bus;

    switch (swi_num) {
        case 0x00: // SoftReset
            arm7tdmi_reset(cpu);
            bios_finish_boot(gba);
            break;

        case 0x01: // RegisterRamReset
            {
                uint8_t flags = cpu->r[0] & 0xFF;
                if (flags & (1 << 0)) memset(bus->ewram, 0, sizeof(bus->ewram));
                if (flags & (1 << 1)) memset(bus->iwram, 0, 0x7E00);
                if (flags & (1 << 2)) memset(bus->palette, 0, sizeof(bus->palette));
                if (flags & (1 << 3)) memset(bus->vram, 0, sizeof(bus->vram));
                if (flags & (1 << 4)) memset(bus->oam, 0, sizeof(bus->oam));
            }
            break;

        case 0x02: // Halt
            cpu->halted = true;
            break;

        case 0x03: // Stop
            cpu->halted = true;
            break;

        case 0x04: // IntrWait
            // Clear stale flags before halting
            if (cpu->r[0] != 0) {
                *(uint16_t*)&bus->iwram[0x7FF8] &= ~cpu->r[1];
            }
            cpu->halted = true;
            break;

        case 0x05: // VBlankIntrWait
            // Clear stale VBlank flag before halting
            *(uint16_t*)&bus->iwram[0x7FF8] &= ~INTR_VBLANK;
            cpu->halted = true;
            break;

        case 0x06: // Div
            {
                int32_t num = (int32_t)cpu->r[0];
                int32_t den = (int32_t)cpu->r[1];
                if (den == 0) {
                    cpu->r[0] = (num >= 0) ? 0x7FFFFFFF : 0x80000000;
                    cpu->r[1] = 0;
                    cpu->r[3] = 0x7FFFFFFF;
                } else {
                    cpu->r[0] = (uint32_t)(num / den);
                    cpu->r[1] = (uint32_t)(num % den);
                    cpu->r[3] = (uint32_t)abs(num / den);
                }
            }
            break;

        case 0x07: // DivArm
            {
                int32_t den = (int32_t)cpu->r[0];
                int32_t num = (int32_t)cpu->r[1];
                if (den == 0) {
                    cpu->r[0] = (num >= 0) ? 0x7FFFFFFF : 0x80000000;
                    cpu->r[1] = 0;
                    cpu->r[3] = 0x7FFFFFFF;
                } else {
                    cpu->r[0] = (uint32_t)(num / den);
                    cpu->r[1] = (uint32_t)(num % den);
                    cpu->r[3] = (uint32_t)abs(num / den);
                }
            }
            break;

        case 0x08: // Sqrt
            {
                uint32_t val = cpu->r[0];
                uint32_t res = 0;
                uint32_t bit = 1u << 30;
                while (bit > val) bit >>= 2;
                while (bit != 0) {
                    if (val >= res + bit) {
                        val -= res + bit;
                        res = (res >> 1) + bit;
                    } else {
                        res >>= 1;
                    }
                    bit >>= 2;
                }
                cpu->r[0] = res;
            }
            break;

        case 0x09: // ArcTan
            {
                int16_t x = (int16_t)cpu->r[0];
                double rad = atan((double)x / 16384.0);
                cpu->r[0] = (uint32_t)(int16_t)(rad * 32768.0 / 3.141592653589793);
            }
            break;

        case 0x0A: // ArcTan2
            {
                int16_t x = (int16_t)cpu->r[0];
                int16_t y = (int16_t)cpu->r[1];
                double rad = atan2((double)y, (double)x);
                cpu->r[0] = (uint32_t)(uint16_t)(rad * 32768.0 / 3.141592653589793);
            }
            break;

        case 0x0B: // CpuSet
            {
                uint32_t src = cpu->r[0];
                uint32_t dst = cpu->r[1];
                uint32_t cnt = cpu->r[2];
                uint32_t count = cnt & 0x1FFFFF;
                bool is_32bit = (cnt & (1 << 26)) != 0;
                bool is_fill = (cnt & (1 << 24)) != 0;

                for (uint32_t i = 0; i < count; i++) {
                    if (is_32bit) {
                        uint32_t val = bus_read32(bus, src);
                        bus_write32(bus, dst, val);
                        dst += 4;
                        if (!is_fill) src += 4;
                    } else {
                        uint16_t val = bus_read16(bus, src);
                        bus_write16(bus, dst, val);
                        dst += 2;
                        if (!is_fill) src += 2;
                    }
                }
            }
            break;

        case 0x0C: // CpuFastSet
            {
                uint32_t src = cpu->r[0];
                uint32_t dst = cpu->r[1];
                uint32_t cnt = cpu->r[2];
                uint32_t count = cnt & 0x1FFFFF;
                bool is_fill = (cnt & (1 << 24)) != 0;

                // Transferred in 8-word chunks
                count = (count + 7) & ~7u;
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t val = bus_read32(bus, src);
                    bus_write32(bus, dst, val);
                    dst += 4;
                    if (!is_fill) src += 4;
                }
            }
            break;

        case 0x11: // LZ77UnCompWram
        case 0x12: // LZ77UnCompVram
            {
                // CRITICAL: VRAM Decompression Buffering
                // Must decompress into a RAM staging buffer and commit full 16-bit halfwords to VRAM
                uint32_t src = cpu->r[0];
                uint32_t dst = cpu->r[1];
                uint32_t header = bus_read32(bus, src);
                src += 4;

                uint32_t decomp_len = header >> 8;
                uint8_t *staging = (uint8_t*)malloc(decomp_len + 32);
                if (!staging) break;

                uint32_t write_pos = 0;
                while (write_pos < decomp_len) {
                    uint8_t flags = bus_read8(bus, src++);
                    for (int bit = 7; bit >= 0 && write_pos < decomp_len; bit--) {
                        if (flags & (1 << bit)) {
                            uint8_t b1 = bus_read8(bus, src++);
                            uint8_t b2 = bus_read8(bus, src++);
                            int len = (b1 >> 4) + 3;
                            int disp = (((b1 & 0xF) << 8) | b2) + 1;
                            int copy_pos = (int)write_pos - disp;
                            for (int k = 0; k < len && write_pos < decomp_len; k++) {
                                staging[write_pos++] = staging[copy_pos++];
                            }
                        } else {
                            staging[write_pos++] = bus_read8(bus, src++);
                        }
                    }
                }

                // Commit staging buffer to destination
                if (swi_num == 0x12) {
                    // Commit 16-bit halfwords to VRAM to prevent 8-bit bus corruption
                    for (uint32_t i = 0; i < decomp_len; i += 2) {
                        uint16_t hw = staging[i] | ((i + 1 < decomp_len) ? ((uint16_t)staging[i + 1] << 8) : 0);
                        bus_write16(bus, dst + i, hw);
                    }
                } else {
                    for (uint32_t i = 0; i < decomp_len; i++) {
                        bus_write8(bus, dst + i, staging[i]);
                    }
                }
                free(staging);
            }
            break;

        case 0x14: // RLUnCompWram
        case 0x15: // RLUnCompVram
            {
                uint32_t src = cpu->r[0];
                uint32_t dst = cpu->r[1];
                uint32_t header = bus_read32(bus, src);
                src += 4;
                uint32_t decomp_len = header >> 8;

                uint8_t *staging = (uint8_t*)malloc(decomp_len + 32);
                if (!staging) break;

                uint32_t write_pos = 0;
                while (write_pos < decomp_len) {
                    uint8_t flag = bus_read8(bus, src++);
                    bool compressed = (flag & 0x80) != 0;
                    int len = (flag & 0x7F) + (compressed ? 3 : 1);
                    if (compressed) {
                        uint8_t val = bus_read8(bus, src++);
                        for (int k = 0; k < len && write_pos < decomp_len; k++) {
                            staging[write_pos++] = val;
                        }
                    } else {
                        for (int k = 0; k < len && write_pos < decomp_len; k++) {
                            staging[write_pos++] = bus_read8(bus, src++);
                        }
                    }
                }

                if (swi_num == 0x15) {
                    for (uint32_t i = 0; i < decomp_len; i += 2) {
                        uint16_t hw = staging[i] | ((i + 1 < decomp_len) ? ((uint16_t)staging[i + 1] << 8) : 0);
                        bus_write16(bus, dst + i, hw);
                    }
                } else {
                    for (uint32_t i = 0; i < decomp_len; i++) {
                        bus_write8(bus, dst + i, staging[i]);
                    }
                }
                free(staging);
            }
            break;

        case 0x19: // SoundBias
            bus->io[0x88] = (uint8_t)cpu->r[0];
            bus->io[0x89] = (uint8_t)(cpu->r[0] >> 8);
            break;

        default:
            break;
    }
}
