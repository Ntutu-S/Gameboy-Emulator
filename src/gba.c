#include "gba.h"
#include <stdio.h>
#include <string.h>

/**
 * Game Boy Advance Master Coordinator
 * "Ntutu Advance System"
 */

void gba_init(GBA *gba) {
    if (!gba) return;
    memset(gba, 0, sizeof(*gba));

    bus_init(&gba->bus, gba);
    arm7tdmi_init(&gba->cpu, &gba->bus);
    ppu_init(&gba->ppu, gba);
    apu_init(&gba->apu, gba);
    dma_init(&gba->dma, gba);
    timer_init(&gba->timer, gba);
    interrupt_init(&gba->intr, gba);
    keypad_init(&gba->keypad, gba);
    bios_init(&gba->bios, gba);

    gba->running = true;
    gba->paused = false;
    gba->skip_intro = false;
}

bool gba_load_rom(GBA *gba, const char *rom_path) {
    if (!gba) return false;
    return bus_load_rom(&gba->bus, rom_path);
}

void gba_reset(GBA *gba, bool skip_intro) {
    if (!gba) return;

    gba->skip_intro = skip_intro;
    arm7tdmi_reset(&gba->cpu);
    ppu_reset(&gba->ppu);
    apu_reset(&gba->apu);
    dma_reset(&gba->dma);
    timer_reset(&gba->timer);
    interrupt_reset(&gba->intr);
    keypad_reset(&gba->keypad);
    bios_init(&gba->bios, gba);

    if (skip_intro) {
        bios_finish_boot(gba);
    }
}

void gba_step(GBA *gba) {
    if (!gba) return;

    int cycles = arm7tdmi_step(&gba->cpu);
    ppu_step(&gba->ppu, cycles);
    timer_step(&gba->timer, cycles);
    apu_step(&gba->apu, cycles);
}

void gba_run_frame(GBA *gba) {
    if (!gba) return;

    if (gba->bios.intro_active) {
        if (bios_render_intro_frame(&gba->bios)) {
            apu_step(&gba->apu, 280896);
            return;
        }
    }

    // 280,896 cycles per frame (1232 cycles/line * 228 lines)
    const int frame_cycles = 280896;
    int cycles_done = 0;

    while (cycles_done < frame_cycles) {
        int cycles = arm7tdmi_step(&gba->cpu);
        ppu_step(&gba->ppu, cycles);
        timer_step(&gba->timer, cycles);
        apu_step(&gba->apu, cycles);
        cycles_done += cycles;
    }
}

void gba_cleanup(GBA *gba) {
    if (!gba) return;
    bus_cleanup(&gba->bus);
    frontend_cleanup(&gba->frontend);
}
