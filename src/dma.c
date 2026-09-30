#include "dma.h"
#include "interrupt.h"
#include "bus.h"
#include "gba.h"
#include <string.h>

/**
 * Direct Memory Access (DMA) Subsystem (Channels 0 - 3)
 * "Ntutu Advance System"
 */

void dma_init(DMA *dma, struct GBA *gba) {
    if (!dma) return;
    memset(dma, 0, sizeof(*dma));
    dma->gba = gba;
    dma_reset(dma);
}

void dma_reset(DMA *dma) {
    if (!dma) return;
    for (int i = 0; i < 4; i++) {
        dma->ch[i].src = 0;
        dma->ch[i].dst = 0;
        dma->ch[i].count = 0;
        dma->ch[i].cnt_h = 0;
        dma->ch[i].current_src = 0;
        dma->ch[i].current_dst = 0;
        dma->ch[i].current_count = 0;
        dma->ch[i].enabled = false;
        dma->ch[i].dst_adj = 0;
        dma->ch[i].src_adj = 0;
        dma->ch[i].repeat = false;
        dma->ch[i].is_32bit = false;
        dma->ch[i].timing = 0;
        dma->ch[i].irq_on_end = false;
    }
}

void dma_run_channel(DMA *dma, int ch_num) {
    DMAChannel *ch = &dma->ch[ch_num];
    if (!ch->enabled) return;

    GBABus *bus = &dma->gba->bus;
    uint32_t step = ch->is_32bit ? 4 : 2;
    uint32_t count = ch->current_count;
    if (count == 0) {
        count = (ch_num == 3) ? 0x10000 : 0x4000;
    }

    // Special handling for EEPROM over DMA3
    if (ch_num == 3 && ((ch->current_src >> 24) == 0x0D || (ch->current_dst >> 24) == 0x0D)) {
        if ((ch->current_dst >> 24) == 0x0D) {
            // Write request to EEPROM
            for (uint32_t i = 0; i < count; i++) {
                uint16_t val = bus_read16(bus, ch->current_src);
                eeprom_write(bus, val, count);
                ch->current_src += 2;
            }
        } else {
            // Read request from EEPROM
            for (uint32_t i = 0; i < count; i++) {
                uint16_t val = eeprom_read(bus);
                bus_write16(bus, ch->current_dst, val);
                ch->current_dst += 2;
            }
        }
    } else {
        // Standard memory block transfer
        for (uint32_t i = 0; i < count; i++) {
            if (ch->is_32bit) {
                uint32_t val = bus_read32(bus, ch->current_src);
                bus_write32(bus, ch->current_dst, val);
            } else {
                uint16_t val = bus_read16(bus, ch->current_src);
                bus_write16(bus, ch->current_dst, val);
            }

            // Adjust source address
            switch (ch->src_adj) {
                case 0: ch->current_src += step; break; // Increment
                case 1: ch->current_src -= step; break; // Decrement
                case 2: break;                          // Fixed
            }

            // Adjust destination address
            switch (ch->dst_adj) {
                case 0: ch->current_dst += step; break; // Increment
                case 1: ch->current_dst -= step; break; // Decrement
                case 2: break;                          // Fixed
                case 3: ch->current_dst += step; break; // Increment (Reload on repeat)
            }
        }
    }

    // Interrupt on completion
    if (ch->irq_on_end && dma->gba) {
        interrupt_raise(&dma->gba->intr, INTR_DMA0 << ch_num);
    }

    // Repeat mode or disable
    if (ch->repeat && ch->timing != DMA_TRIGGER_IMMEDIATE) {
        ch->current_count = ch->count;
        if (ch->dst_adj == 3) {
            ch->current_dst = ch->dst; // Reload destination address
        }
    } else {
        ch->enabled = false;
        ch->cnt_h &= ~(1 << 15);
    }
}

void dma_trigger(DMA *dma, int trigger) {
    if (!dma) return;
    // Priority arbitration: DMA0 > DMA1 > DMA2 > DMA3
    for (int i = 0; i < 4; i++) {
        if (dma->ch[i].enabled && dma->ch[i].timing == trigger) {
            dma_run_channel(dma, i);
        }
    }
}

uint16_t dma_read_cnt_h(DMA *dma, int ch) {
    if (!dma || ch < 0 || ch > 3) return 0;
    return dma->ch[ch].cnt_h;
}

void dma_write_src(DMA *dma, int ch, uint32_t val, int offset) {
    if (!dma || ch < 0 || ch > 3) return;
    if (offset == 0) {
        dma->ch[ch].src = (dma->ch[ch].src & 0xFFFF0000) | (val & 0xFFFF);
    } else {
        dma->ch[ch].src = (dma->ch[ch].src & 0x0000FFFF) | (val & 0xFFFF0000);
    }
}

void dma_write_dst(DMA *dma, int ch, uint32_t val, int offset) {
    if (!dma || ch < 0 || ch > 3) return;
    if (offset == 0) {
        dma->ch[ch].dst = (dma->ch[ch].dst & 0xFFFF0000) | (val & 0xFFFF);
    } else {
        dma->ch[ch].dst = (dma->ch[ch].dst & 0x0000FFFF) | (val & 0xFFFF0000);
    }
}

void dma_write_count(DMA *dma, int ch, uint16_t val) {
    if (!dma || ch < 0 || ch > 3) return;
    dma->ch[ch].count = val;
}

void dma_write_cnt_h(DMA *dma, int ch, uint16_t val) {
    if (!dma || ch < 0 || ch > 3) return;
    DMAChannel *c = &dma->ch[ch];
    bool was_enabled = c->enabled;

    c->cnt_h = val;
    c->dst_adj = (val >> 5) & 3;
    c->src_adj = (val >> 7) & 3;
    c->repeat = (val & (1 << 9)) != 0;
    c->is_32bit = (val & (1 << 10)) != 0;
    c->timing = (val >> 12) & 3;
    c->irq_on_end = (val & (1 << 14)) != 0;
    c->enabled = (val & (1 << 15)) != 0;

    if (!was_enabled && c->enabled) {
        c->current_src = c->src;
        c->current_dst = c->dst;
        c->current_count = c->count;

        // Immediate start
        if (c->timing == DMA_TRIGGER_IMMEDIATE) {
            dma_run_channel(dma, ch);
        }
    }
}
