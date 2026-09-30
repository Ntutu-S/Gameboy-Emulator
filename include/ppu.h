#ifndef PPU_H
#define PPU_H

#include <stdint.h>
#include <stdbool.h>

struct GBA;

#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 160
#define VBLANK_LINES  68
#define TOTAL_LINES   228

#define HDRAW_CYCLES  960
#define HBLANK_CYCLES 272
#define SCANLINE_CYCLES (HDRAW_CYCLES + HBLANK_CYCLES) // 1232 cycles

typedef struct AffineBG {
    int16_t pa, pb, pc, pd; // 8.8 fixed-point
    int32_t x, y;           // 19.8 fixed-point (from registers)
    int32_t current_x;      // Latched internal tracking coordinate
    int32_t current_y;
} AffineBG;

typedef struct PPU {
    uint32_t framebuffer[SCREEN_HEIGHT * SCREEN_WIDTH]; // ARGB8888

    // Registers
    uint16_t dispcnt;
    uint16_t dispstat;
    uint16_t vcount;

    uint16_t bgcnt[4];
    uint16_t bghofs[4];
    uint16_t bgvofs[4];

    AffineBG affine[2]; // Affine parameters for BG2 and BG3

    uint16_t win0h, win1h;
    uint16_t win0v, win1v;
    uint16_t winin, winout;

    uint16_t bldcnt;
    uint16_t bldalpha;
    uint16_t bldy;

    // Scanline & cycle counters
    int scanline_cycles;
    bool in_hblank;
    bool in_vblank;

    struct GBA *gba;
} PPU;

void ppu_init(PPU *ppu, struct GBA *gba);
void ppu_reset(PPU *ppu);
void ppu_step(PPU *ppu, int cycles);
void ppu_render_scanline(PPU *ppu);

uint16_t ppu_read_reg(PPU *ppu, uint32_t addr);
void ppu_write_reg(PPU *ppu, uint32_t addr, uint16_t val);

#endif // PPU_H
