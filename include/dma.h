#ifndef DMA_H
#define DMA_H

#include <stdint.h>
#include <stdbool.h>

struct GBA;

#define DMA_TRIGGER_IMMEDIATE 0
#define DMA_TRIGGER_VBLANK    1
#define DMA_TRIGGER_HBLANK    2
#define DMA_TRIGGER_SPECIAL   3

typedef struct DMAChannel {
    uint32_t src;
    uint32_t dst;
    uint16_t count;
    uint16_t cnt_h;

    // Internal latched registers
    uint32_t current_src;
    uint32_t current_dst;
    uint32_t current_count;

    bool enabled;
    int dst_adj;      // 0: Inc, 1: Dec, 2: Fixed, 3: Reload
    int src_adj;      // 0: Inc, 1: Dec, 2: Fixed
    bool repeat;
    bool is_32bit;
    int timing;       // 0: Imm, 1: VBlank, 2: HBlank, 3: Special
    bool irq_on_end;
} DMAChannel;

typedef struct DMA {
    DMAChannel ch[4];
    struct GBA *gba;
} DMA;

void dma_init(DMA *dma, struct GBA *gba);
void dma_reset(DMA *dma);
void dma_trigger(DMA *dma, int trigger);
void dma_run_channel(DMA *dma, int ch);

uint16_t dma_read_cnt_h(DMA *dma, int ch);
void dma_write_src(DMA *dma, int ch, uint32_t val, int offset);
void dma_write_dst(DMA *dma, int ch, uint32_t val, int offset);
void dma_write_count(DMA *dma, int ch, uint16_t val);
void dma_write_cnt_h(DMA *dma, int ch, uint16_t val);

#endif // DMA_H
