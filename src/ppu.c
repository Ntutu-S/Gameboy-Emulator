#include "ppu.h"
#include "interrupt.h"
#include "dma.h"
#include "bus.h"
#include "gba.h"
#include <string.h>
#include <stdlib.h>

/**
 * Game Boy Advance Picture Processing Unit (PPU)
 * "Ntutu Advance System"
 */

// Structure to track compositing information per-pixel on a scanline
typedef struct PixelInfo {
    uint32_t color;   // 32-bit ARGB8888 color
    int priority;     // 0 - 3 (0 is highest), 4 = backdrop / transparent
    int layer;        // 0: BG0, 1: BG1, 2: BG2, 3: BG3, 4: OBJ, 5: BD (backdrop)
    bool is_semi_transparent;
} PixelInfo;

static const int sprite_sizes[3][4][2] = {
    // Square: 8x8, 16x16, 32x32, 64x64
    { {8, 8}, {16, 16}, {32, 32}, {64, 64} },
    // Horizontal: 16x8, 32x8, 32x16, 64x32
    { {16, 8}, {32, 8}, {32, 16}, {64, 32} },
    // Vertical: 8x16, 8x32, 16x32, 32x64
    { {8, 16}, {8, 32}, {16, 32}, {32, 64} }
};

static inline uint32_t rgb555_to_argb8888(uint16_t c) {
    uint32_t r = c & 0x1F;
    uint32_t g = (c >> 5) & 0x1F;
    uint32_t b = (c >> 10) & 0x1F;

    uint32_t r8 = (r << 3) | (r >> 2);
    uint32_t g8 = (g << 3) | (g >> 2);
    uint32_t b8 = (b << 3) | (b >> 2);

    return 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
}

void ppu_init(PPU *ppu, struct GBA *gba) {
    if (!ppu) return;
    memset(ppu, 0, sizeof(*ppu));
    ppu->gba = gba;
    ppu_reset(ppu);
}

void ppu_reset(PPU *ppu) {
    if (!ppu) return;
    memset(ppu->framebuffer, 0, sizeof(ppu->framebuffer));
    ppu->dispcnt = 0;
    ppu->dispstat = 0;
    ppu->vcount = 0;
    memset(ppu->bgcnt, 0, sizeof(ppu->bgcnt));
    memset(ppu->bghofs, 0, sizeof(ppu->bghofs));
    memset(ppu->bgvofs, 0, sizeof(ppu->bgvofs));
    memset(ppu->affine, 0, sizeof(ppu->affine));
    ppu->win0h = ppu->win1h = 0;
    ppu->win0v = ppu->win1v = 0;
    ppu->winin = ppu->winout = 0;
    ppu->bldcnt = ppu->bldalpha = ppu->bldy = 0;
    ppu->scanline_cycles = 0;
    ppu->in_hblank = false;
    ppu->in_vblank = false;
}

static void render_text_bg(PPU *ppu, int bg_idx, PixelInfo *line_buf) {
    GBABus *bus = &ppu->gba->bus;
    uint16_t cnt = ppu->bgcnt[bg_idx];
    int priority = cnt & 3;
    int char_block = (cnt >> 2) & 3;
    bool mosaic = (cnt & (1 << 6)) != 0;
    (void)mosaic;
    bool color_256 = (cnt & (1 << 7)) != 0;
    int screen_block = (cnt >> 8) & 0x1F;
    int size = (cnt >> 14) & 3;

    int screen_w = 256;
    int screen_h = 256;
    if (size == 1) screen_w = 512;
    else if (size == 2) screen_h = 512;
    else if (size == 3) { screen_w = 512; screen_h = 512; }

    int hofs = ppu->bghofs[bg_idx] & 0x1FF;
    int vofs = ppu->bgvofs[bg_idx] & 0x1FF;
    int py = (ppu->vcount + vofs) % screen_h;

    uint32_t char_base = char_block * 0x4000;
    uint32_t screen_base = screen_block * 0x800;

    for (int x = 0; x < SCREEN_WIDTH; x++) {
        int px = (x + hofs) % screen_w;

        // Screen block indexing for 512x256, 256x512, 512x512
        int sb_x = px / 256;
        int sb_y = py / 256;
        int current_sb = screen_base;
        if (size == 1) current_sb += sb_x * 0x800;
        else if (size == 2) current_sb += sb_y * 0x800;
        else if (size == 3) current_sb += (sb_x + sb_y * 2) * 0x800;

        int tile_x = (px % 256) / 8;
        int tile_y = (py % 256) / 8;
        uint32_t map_addr = current_sb + (tile_y * 32 + tile_x) * 2;
        if (map_addr + 1 >= 0x18000) continue;

        uint16_t tile_entry = *(uint16_t*)&bus->vram[map_addr];
        int tile_num = tile_entry & 0x3FF;
        bool hflip = (tile_entry & (1 << 10)) != 0;
        bool vflip = (tile_entry & (1 << 11)) != 0;
        int pal_num = (tile_entry >> 12) & 0xF;

        int in_tile_x = hflip ? (7 - (px % 8)) : (px % 8);
        int in_tile_y = vflip ? (7 - (py % 8)) : (py % 8);

        uint8_t color_idx = 0;
        if (!color_256) {
            // 16-color (4 bpp)
            uint32_t tile_data_addr = char_base + tile_num * 32 + in_tile_y * 4 + (in_tile_x / 2);
            if (tile_data_addr < 0x18000) {
                uint8_t byte = bus->vram[tile_data_addr];
                color_idx = (in_tile_x & 1) ? (byte >> 4) : (byte & 0x0F);
            }
            if (color_idx != 0) {
                uint16_t pal_entry = *(uint16_t*)&bus->palette[(pal_num * 16 + color_idx) * 2];
                // CRITICAL Compositor Precedence: OBJ > BG0 > BG1 > BG2 > BG3
                if (priority < line_buf[x].priority || (priority == line_buf[x].priority && bg_idx < line_buf[x].layer)) {
                    line_buf[x].color = rgb555_to_argb8888(pal_entry);
                    line_buf[x].priority = priority;
                    line_buf[x].layer = bg_idx;
                }
            }
        } else {
            // 256-color (8 bpp)
            uint32_t tile_data_addr = char_base + tile_num * 64 + in_tile_y * 8 + in_tile_x;
            if (tile_data_addr < 0x18000) {
                color_idx = bus->vram[tile_data_addr];
            }
            if (color_idx != 0) {
                uint16_t pal_entry = *(uint16_t*)&bus->palette[color_idx * 2];
                if (priority < line_buf[x].priority || (priority == line_buf[x].priority && bg_idx < line_buf[x].layer)) {
                    line_buf[x].color = rgb555_to_argb8888(pal_entry);
                    line_buf[x].priority = priority;
                    line_buf[x].layer = bg_idx;
                }
            }
        }
    }
}

static void render_affine_bg(PPU *ppu, int bg_idx, PixelInfo *line_buf) {
    GBABus *bus = &ppu->gba->bus;
    uint16_t cnt = ppu->bgcnt[bg_idx];
    int priority = cnt & 3;
    int char_block = (cnt >> 2) & 3;
    int screen_block = (cnt >> 8) & 0x1F;
    bool wrap = (cnt & (1 << 13)) != 0;
    int size_code = (cnt >> 14) & 3;
    int size = 128 << size_code; // 128, 256, 512, 1024

    AffineBG *aff = &ppu->affine[bg_idx - 2];
    int32_t cx = aff->current_x;
    int32_t cy = aff->current_y;

    uint32_t char_base = char_block * 0x4000;
    uint32_t screen_base = screen_block * 0x800;

    for (int x = 0; x < SCREEN_WIDTH; x++) {
        int px = cx >> 8;
        int py = cy >> 8;

        cx += aff->pa;
        cy += aff->pc;

        if (wrap) {
            px = (px % size + size) % size;
            py = (py % size + size) % size;
        } else {
            if (px < 0 || px >= size || py < 0 || py >= size) continue;
        }

        int tile_x = px / 8;
        int tile_y = py / 8;
        int tiles_per_row = size / 8;
        uint32_t map_addr = screen_base + tile_y * tiles_per_row + tile_x;
        if (map_addr >= 0x18000) continue;

        uint8_t tile_num = bus->vram[map_addr];
        uint32_t tile_addr = char_base + tile_num * 64 + (py % 8) * 8 + (px % 8);
        if (tile_addr >= 0x18000) continue;

        uint8_t color_idx = bus->vram[tile_addr];
        if (color_idx != 0) {
            uint16_t pal_entry = *(uint16_t*)&bus->palette[color_idx * 2];
            if (priority < line_buf[x].priority || (priority == line_buf[x].priority && bg_idx < line_buf[x].layer)) {
                line_buf[x].color = rgb555_to_argb8888(pal_entry);
                line_buf[x].priority = priority;
                line_buf[x].layer = bg_idx;
            }
        }
    }
}

static void render_sprites(PPU *ppu, PixelInfo *line_buf) {
    GBABus *bus = &ppu->gba->bus;
    bool is_1d = (ppu->dispcnt & (1 << 6)) != 0;
    int y = ppu->vcount;

    // Sprite Tie-Breaking Rule: When two sprites overlap with the same priority,
    // lower OAM index has precedence. We iterate backwards (127 down to 0) so
    // earlier sprites overwrite later sprites.
    for (int i = 127; i >= 0; i--) {
        uint16_t attr0 = *(uint16_t*)&bus->oam[i * 8 + 0];
        uint16_t attr1 = *(uint16_t*)&bus->oam[i * 8 + 2];
        uint16_t attr2 = *(uint16_t*)&bus->oam[i * 8 + 4];

        int obj_mode = (attr0 >> 10) & 3;
        if (obj_mode == 2) continue; // Skip OBJ window in main render

        bool is_affine = (attr0 & (1 << 8)) != 0;
        bool is_double = (attr0 & (1 << 9)) != 0;
        if (!is_affine && is_double) continue; // Sprite disabled

        int shape = (attr0 >> 14) & 3;
        int size_idx = (attr1 >> 14) & 3;
        if (shape == 3) continue;

        int width = sprite_sizes[shape][size_idx][0];
        int height = sprite_sizes[shape][size_idx][1];

        int py = attr0 & 0xFF;
        if (py >= 160) py -= 256;

        int px = attr1 & 0x1FF;
        if (px >= 240) px -= 512;

        int bound_w = width;
        int bound_h = height;
        if (is_affine && is_double) {
            bound_w *= 2;
            bound_h *= 2;
        }

        if (y < py || y >= py + bound_h) continue;

        bool color_256 = (attr0 & (1 << 13)) != 0;
        int base_tile = attr2 & 0x3FF;
        int priority = (attr2 >> 10) & 3;
        int pal_num = (attr2 >> 12) & 0xF;

        if (!is_affine) {
            bool hflip = (attr1 & (1 << 12)) != 0;
            bool vflip = (attr1 & (1 << 13)) != 0;
            int spr_y = vflip ? (height - 1 - (y - py)) : (y - py);

            for (int x = 0; x < width; x++) {
                int screen_x = px + x;
                if (screen_x < 0 || screen_x >= SCREEN_WIDTH) continue;

                int spr_x = hflip ? (width - 1 - x) : x;
                int tile_x = spr_x / 8;
                int tile_y = spr_y / 8;
                int in_tile_x = spr_x % 8;
                int in_tile_y = spr_y % 8;

                uint32_t tile_idx;
                if (is_1d) {
                    int tiles_w = width / 8;
                    tile_idx = base_tile + (tile_y * tiles_w + tile_x) * (color_256 ? 2 : 1);
                } else {
                    tile_idx = base_tile + tile_y * 32 + tile_x * (color_256 ? 2 : 1);
                }

                uint32_t tile_addr = 0x10000 + tile_idx * 32;
                uint8_t color_idx = 0;

                if (!color_256) {
                    tile_addr += in_tile_y * 4 + (in_tile_x / 2);
                    if (tile_addr < 0x18000) {
                        uint8_t byte = bus->vram[tile_addr];
                        color_idx = (in_tile_x & 1) ? (byte >> 4) : (byte & 0x0F);
                    }
                } else {
                    tile_addr += in_tile_y * 8 + in_tile_x;
                    if (tile_addr < 0x18000) {
                        color_idx = bus->vram[tile_addr];
                    }
                }

                if (color_idx != 0) {
                    uint16_t pal_entry = *(uint16_t*)&bus->palette[0x200 + (color_256 ? color_idx : (pal_num * 16 + color_idx)) * 2];

                    // CRITICAL Compositor Priority Precedence Rule:
                    // OBJ takes precedence over BG at equal priority: OBJ > BG0 > BG1 > BG2 > BG3
                    if (priority <= line_buf[screen_x].priority) {
                        line_buf[screen_x].color = rgb555_to_argb8888(pal_entry);
                        line_buf[screen_x].priority = priority;
                        line_buf[screen_x].layer = 4; // OBJ layer
                        line_buf[screen_x].is_semi_transparent = (obj_mode == 1);
                    }
                }
            }
        }
    }
}

void ppu_render_scanline(PPU *ppu) {
    if (!ppu || !ppu->gba) return;
    int y = ppu->vcount;
    if (y >= SCREEN_HEIGHT) return;

    GBABus *bus = &ppu->gba->bus;
    uint32_t *dest = &ppu->framebuffer[y * SCREEN_WIDTH];

    // Initialize line buffer with backdrop color (Palette entry 0)
    uint16_t backdrop_entry = *(uint16_t*)&bus->palette[0];
    uint32_t backdrop_color = rgb555_to_argb8888(backdrop_entry);

    PixelInfo line_buf[SCREEN_WIDTH];
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        line_buf[x].color = backdrop_color;
        line_buf[x].priority = 4; // Lowest priority
        line_buf[x].layer = 5;    // Backdrop
        line_buf[x].is_semi_transparent = false;
    }

    // Forced blank
    if (ppu->dispcnt & (1 << 7)) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            dest[x] = 0xFFFFFFFF; // White screen
        }
        return;
    }

    int mode = ppu->dispcnt & 7;

    switch (mode) {
        case 0: // Mode 0: 4 Text backgrounds
            if (ppu->dispcnt & (1 << 8))  render_text_bg(ppu, 0, line_buf);
            if (ppu->dispcnt & (1 << 9))  render_text_bg(ppu, 1, line_buf);
            if (ppu->dispcnt & (1 << 10)) render_text_bg(ppu, 2, line_buf);
            if (ppu->dispcnt & (1 << 11)) render_text_bg(ppu, 3, line_buf);
            break;

        case 1: // Mode 1: Text BG0, BG1 + Affine BG2
            if (ppu->dispcnt & (1 << 8))  render_text_bg(ppu, 0, line_buf);
            if (ppu->dispcnt & (1 << 9))  render_text_bg(ppu, 1, line_buf);
            if (ppu->dispcnt & (1 << 10)) render_affine_bg(ppu, 2, line_buf);
            break;

        case 2: // Mode 2: Affine BG2, BG3
            if (ppu->dispcnt & (1 << 10)) render_affine_bg(ppu, 2, line_buf);
            if (ppu->dispcnt & (1 << 11)) render_affine_bg(ppu, 3, line_buf);
            break;

        case 3: // Mode 3: 240x160 16-bit RGB555 direct color bitmap
            if (ppu->dispcnt & (1 << 10)) {
                for (int x = 0; x < SCREEN_WIDTH; x++) {
                    uint16_t c = *(uint16_t*)&bus->vram[(y * 240 + x) * 2];
                    line_buf[x].color = rgb555_to_argb8888(c);
                    line_buf[x].priority = ppu->bgcnt[2] & 3;
                    line_buf[x].layer = 2;
                }
            }
            break;

        case 4: // Mode 4: 240x160 8-bit palette-indexed bitmap with page flipping
            if (ppu->dispcnt & (1 << 10)) {
                uint32_t page_offset = (ppu->dispcnt & (1 << 4)) ? 0xA000 : 0x0000;
                for (int x = 0; x < SCREEN_WIDTH; x++) {
                    uint8_t pal_idx = bus->vram[page_offset + y * 240 + x];
                    if (pal_idx != 0) {
                        uint16_t c = *(uint16_t*)&bus->palette[pal_idx * 2];
                        line_buf[x].color = rgb555_to_argb8888(c);
                        line_buf[x].priority = ppu->bgcnt[2] & 3;
                        line_buf[x].layer = 2;
                    }
                }
            }
            break;

        case 5: // Mode 5: 160x128 16-bit RGB555 direct color bitmap with page flipping
            if ((ppu->dispcnt & (1 << 10)) && y < 128) {
                uint32_t page_offset = (ppu->dispcnt & (1 << 4)) ? 0xA000 : 0x0000;
                for (int x = 0; x < 160; x++) {
                    uint16_t c = *(uint16_t*)&bus->vram[page_offset + (y * 160 + x) * 2];
                    line_buf[x].color = rgb555_to_argb8888(c);
                    line_buf[x].priority = ppu->bgcnt[2] & 3;
                    line_buf[x].layer = 2;
                }
            }
            break;
    }

    // Render sprites if enabled (bit 12 of DISPCNT)
    if (ppu->dispcnt & (1 << 12)) {
        render_sprites(ppu, line_buf);
    }

    // Commit composited scanline to framebuffer
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        dest[x] = line_buf[x].color;
    }
}

void ppu_step(PPU *ppu, int cycles) {
    if (!ppu || !ppu->gba) return;

    ppu->scanline_cycles += cycles;

    // HDraw to HBlank transition (at 960 cycles)
    if (!ppu->in_hblank && ppu->scanline_cycles >= HDRAW_CYCLES) {
        ppu->in_hblank = true;
        ppu->dispstat |= (1 << 1); // Set HBlank flag

        // Trigger HBlank interrupt if enabled
        if (ppu->dispstat & (1 << 4)) {
            interrupt_raise(&ppu->gba->intr, INTR_HBLANK);
        }

        // Trigger HBlank DMA
        if (ppu->vcount < SCREEN_HEIGHT) {
            dma_trigger(&ppu->gba->dma, DMA_TRIGGER_HBLANK);
        }
    }

    // Scanline completion (at 1232 cycles)
    if (ppu->scanline_cycles >= SCANLINE_CYCLES) {
        ppu->scanline_cycles -= SCANLINE_CYCLES;
        ppu->in_hblank = false;
        ppu->dispstat &= ~(1 << 1); // Clear HBlank flag

        // Advance to next scanline
        ppu->vcount++;

        // VBlank start (at scanline 160)
        if (ppu->vcount == SCREEN_HEIGHT) {
            ppu->in_vblank = true;
            ppu->dispstat |= (1 << 0); // Set VBlank flag

            if (ppu->dispstat & (1 << 3)) {
                interrupt_raise(&ppu->gba->intr, INTR_VBLANK);
            }
            dma_trigger(&ppu->gba->dma, DMA_TRIGGER_VBLANK);
        }

        // Frame completion (at scanline 228)
        if (ppu->vcount >= TOTAL_LINES) {
            ppu->vcount = 0;
            ppu->in_vblank = false;
            ppu->dispstat &= ~(1 << 0); // Clear VBlank flag

            // Latch affine reference coordinates for line 0
            ppu->affine[0].current_x = ppu->affine[0].x;
            ppu->affine[0].current_y = ppu->affine[0].y;
            ppu->affine[1].current_x = ppu->affine[1].x;
            ppu->affine[1].current_y = ppu->affine[1].y;
        }

        // V-Counter Match check
        uint16_t vcount_target = (ppu->dispstat >> 8) & 0xFF;
        if (ppu->vcount == vcount_target) {
            ppu->dispstat |= (1 << 2);
            if (ppu->dispstat & (1 << 5)) {
                interrupt_raise(&ppu->gba->intr, INTR_VCOUNT);
            }
        } else {
            ppu->dispstat &= ~(1 << 2);
        }

        // Render visible scanline
        if (ppu->vcount < SCREEN_HEIGHT) {
            ppu_render_scanline(ppu);

            // Step affine coordinates
            ppu->affine[0].current_x += ppu->affine[0].pb;
            ppu->affine[0].current_y += ppu->affine[0].pd;
            ppu->affine[1].current_x += ppu->affine[1].pb;
            ppu->affine[1].current_y += ppu->affine[1].pd;
        }
    }
}

uint16_t ppu_read_reg(PPU *ppu, uint32_t addr) {
    if (!ppu) return 0;
    switch (addr) {
        case 0x00: return ppu->dispcnt;
        case 0x04: return ppu->dispstat;
        case 0x06: return ppu->vcount;
        case 0x08: return ppu->bgcnt[0];
        case 0x0A: return ppu->bgcnt[1];
        case 0x0C: return ppu->bgcnt[2];
        case 0x0E: return ppu->bgcnt[3];
        case 0x48: return ppu->winin;
        case 0x4A: return ppu->winout;
        case 0x50: return ppu->bldcnt;
        case 0x52: return ppu->bldalpha;
        default:   return *(uint16_t*)&ppu->gba->bus.io[addr];
    }
}

void ppu_write_reg(PPU *ppu, uint32_t addr, uint16_t val) {
    if (!ppu) return;
    switch (addr) {
        case 0x00: ppu->dispcnt = val; break;
        case 0x04: ppu->dispstat = (ppu->dispstat & 0x07) | (val & ~0x07); break;
        case 0x08: ppu->bgcnt[0] = val; break;
        case 0x0A: ppu->bgcnt[1] = val; break;
        case 0x0C: ppu->bgcnt[2] = val; break;
        case 0x0E: ppu->bgcnt[3] = val; break;
        case 0x10: ppu->bghofs[0] = val & 0x1FF; break;
        case 0x12: ppu->bgvofs[0] = val & 0x1FF; break;
        case 0x14: ppu->bghofs[1] = val & 0x1FF; break;
        case 0x16: ppu->bgvofs[1] = val & 0x1FF; break;
        case 0x18: ppu->bghofs[2] = val & 0x1FF; break;
        case 0x1A: ppu->bgvofs[2] = val & 0x1FF; break;
        case 0x1C: ppu->bghofs[3] = val & 0x1FF; break;
        case 0x1E: ppu->bgvofs[3] = val & 0x1FF; break;

        // BG2 Affine params
        case 0x20: ppu->affine[0].pa = (int16_t)val; break;
        case 0x22: ppu->affine[0].pb = (int16_t)val; break;
        case 0x24: ppu->affine[0].pc = (int16_t)val; break;
        case 0x26: ppu->affine[0].pd = (int16_t)val; break;
        case 0x28: ppu->affine[0].x = (ppu->affine[0].x & 0x0FFF0000) | val; ppu->affine[0].current_x = ppu->affine[0].x; break;
        case 0x2A: ppu->affine[0].x = (ppu->affine[0].x & 0x0000FFFF) | ((val & 0x0FFF) << 16); if (val & 0x0800) ppu->affine[0].x |= 0xF0000000; ppu->affine[0].current_x = ppu->affine[0].x; break;
        case 0x2C: ppu->affine[0].y = (ppu->affine[0].y & 0x0FFF0000) | val; ppu->affine[0].current_y = ppu->affine[0].y; break;
        case 0x2E: ppu->affine[0].y = (ppu->affine[0].y & 0x0000FFFF) | ((val & 0x0FFF) << 16); if (val & 0x0800) ppu->affine[0].y |= 0xF0000000; ppu->affine[0].current_y = ppu->affine[0].y; break;

        // BG3 Affine params
        case 0x30: ppu->affine[1].pa = (int16_t)val; break;
        case 0x32: ppu->affine[1].pb = (int16_t)val; break;
        case 0x34: ppu->affine[1].pc = (int16_t)val; break;
        case 0x36: ppu->affine[1].pd = (int16_t)val; break;
        case 0x38: ppu->affine[1].x = (ppu->affine[1].x & 0x0FFF0000) | val; ppu->affine[1].current_x = ppu->affine[1].x; break;
        case 0x3A: ppu->affine[1].x = (ppu->affine[1].x & 0x0000FFFF) | ((val & 0x0FFF) << 16); if (val & 0x0800) ppu->affine[1].x |= 0xF0000000; ppu->affine[1].current_x = ppu->affine[1].x; break;
        case 0x3C: ppu->affine[1].y = (ppu->affine[1].y & 0x0FFF0000) | val; ppu->affine[1].current_y = ppu->affine[1].y; break;
        case 0x3E: ppu->affine[1].y = (ppu->affine[1].y & 0x0000FFFF) | ((val & 0x0FFF) << 16); if (val & 0x0800) ppu->affine[1].y |= 0xF0000000; ppu->affine[1].current_y = ppu->affine[1].y; break;

        case 0x40: ppu->win0h = val; break;
        case 0x42: ppu->win1h = val; break;
        case 0x44: ppu->win0v = val; break;
        case 0x46: ppu->win1v = val; break;
        case 0x48: ppu->winin = val; break;
        case 0x4A: ppu->winout = val; break;
        case 0x50: ppu->bldcnt = val; break;
        case 0x52: ppu->bldalpha = val; break;
        case 0x54: ppu->bldy = val & 0x1F; break;
    }
}
