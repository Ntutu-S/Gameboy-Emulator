#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "gba.h"
#include "frontend.h"

/**
 * Ntutu Advance System - Main Entry Point
 * "ntutu_station" / "ntutu_emu"
 */

static void print_banner(void) {
    printf("=====================================================\n");
    printf("   NTUTU ADVANCE SYSTEM (GBA 32-bit RISC Emulator)   \n");
    printf("     Pure C11 - Native Linux X11/XShm & ALSA Audio   \n");
    printf("=====================================================\n");
}

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options] <path_to_rom.gba>\n\n", prog_name);
    printf("Options:\n");
    printf("  --skip-intro    Bypass Ntutu boot intro sequence\n");
    printf("  --fullscreen, -f Start emulator in fullscreen mode\n");
    printf("  --help, -h      Display this help menu\n\n");
    printf("Controls:\n");
    printf("  A / B           Z / X, A / S\n");
    printf("  L / R Triggers  Q / W (or E)\n");
    printf("  Start / Select  Enter / Space (or Backspace)\n");
    printf("  D-Pad           Up, Down, Left, Right\n");
    printf("  Fast-Forward    Tab (Toggle)\n");
    printf("  Fullscreen      F11 or F\n\n");
}

int main(int argc, char *argv[]) {
    print_banner();

    const char *rom_path = NULL;
    bool skip_intro = false;
    bool start_fullscreen = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--skip-intro") == 0) {
            skip_intro = true;
        } else if (strcmp(argv[i], "--fullscreen") == 0 || strcmp(argv[i], "-f") == 0) {
            start_fullscreen = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            rom_path = argv[i];
        }
    }

    if (!rom_path) {
        printf("[INFO] No ROM specified. Checking for default ROM in roms/ ...\n");
        rom_path = "roms/game.gba";
    }

    GBA gba;
    gba_init(&gba);

    if (!gba_load_rom(&gba, rom_path)) {
        fprintf(stderr, "[ERROR] Could not load ROM from '%s'.\n", rom_path);
        print_usage(argv[0]);
        gba_cleanup(&gba);
        return 1;
    }

    gba_reset(&gba, skip_intro);

    // Initialize native X11/XShm and ALSA frontend
    if (!frontend_init(&gba.frontend, &gba, gba.bus.rom_title)) {
        fprintf(stderr, "[WARNING] GUI initialization failed; falling back to headless execution.\n");
    } else if (start_fullscreen) {
        frontend_toggle_fullscreen(&gba.frontend);
    }

    printf("[SYSTEM] Emulation started for \"%s\"...\n", gba.bus.rom_title);

    // Interactive Emulation Loop
    while (gba.running) {
        // Poll X11 OS events
        if (gba.frontend.display) {
            if (!frontend_poll_events(&gba.frontend)) {
                gba.running = false;
                break;
            }
        }

        // Run full GBA frame
        gba_run_frame(&gba);

        // Blit frame to screen
        if (gba.frontend.display) {
            frontend_render_frame(&gba.frontend, gba.ppu.framebuffer);
        }

        // Output audio samples
        int16_t audio_buf[2048 * 2];
        size_t samples = apu_read_samples(&gba.apu, audio_buf, 2048);
        if (samples > 0 && gba.frontend.audio_enabled) {
            frontend_push_audio(&gba.frontend, audio_buf, samples);
        }

        // Frame timing synchronization (59.73 Hz)
        if (gba.frontend.display) {
            frontend_sync(&gba.frontend);
        }
    }

    printf("[SYSTEM] Shutting down Ntutu Advance System.\n");
    gba_cleanup(&gba);
    return 0;
}
