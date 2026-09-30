#ifndef BIOS_H
#define BIOS_H

#include <stdint.h>
#include <stdbool.h>

struct GBA;

typedef struct BIOS {
    bool intro_active;
    int intro_frame;
    bool skip_intro;
    struct GBA *gba;
} BIOS;

void bios_init(BIOS *bios, struct GBA *gba);
void bios_install_trampoline(struct GBA *gba);
void bios_call(struct GBA *gba, uint8_t swi_number);

bool bios_render_intro_frame(BIOS *bios);
void bios_finish_boot(struct GBA *gba);

#endif // BIOS_H
