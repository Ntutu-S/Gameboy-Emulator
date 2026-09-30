#ifndef GBA_H
#define GBA_H

#include <stdint.h>
#include <stdbool.h>

#include "arm7tdmi.h"
#include "bus.h"
#include "ppu.h"
#include "apu.h"
#include "dma.h"
#include "timer.h"
#include "interrupt.h"
#include "keypad.h"
#include "bios.h"
#include "frontend.h"

typedef struct GBA {
    ARM7TDMI cpu;
    GBABus bus;
    PPU ppu;
    APU apu;
    DMA dma;
    Timer timer;
    Interrupt intr;
    Keypad keypad;
    BIOS bios;
    Frontend frontend;

    bool running;
    bool paused;
    bool skip_intro;
} GBA;

void gba_init(GBA *gba);
bool gba_load_rom(GBA *gba, const char *rom_path);
void gba_reset(GBA *gba, bool skip_intro);
void gba_step(GBA *gba);
void gba_run_frame(GBA *gba);
void gba_cleanup(GBA *gba);

#endif // GBA_H
