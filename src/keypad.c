#include "keypad.h"
#include "interrupt.h"
#include "gba.h"
#include <string.h>

/**
 * GBA Keypad Controller
 * "Ntutu Advance System"
 */

void keypad_init(Keypad *keypad, struct GBA *gba) {
    if (!keypad) return;
    memset(keypad, 0, sizeof(*keypad));
    keypad->gba = gba;
    keypad_reset(keypad);
}

void keypad_reset(Keypad *keypad) {
    if (!keypad) return;
    keypad->keyinput = 0x03FF; // All 10 keys released (active low)
    keypad->keycnt = 0;
}

void keypad_set_key(Keypad *keypad, int key, bool pressed) {
    if (!keypad || key < 0 || key > 9) return;
    if (pressed) {
        keypad->keyinput &= ~(1 << key);
    } else {
        keypad->keyinput |= (1 << key);
    }
    keypad_check_irq(keypad);
}

uint16_t keypad_read_input(const Keypad *keypad) {
    return keypad ? keypad->keyinput : 0x03FF;
}

uint16_t keypad_read_control(const Keypad *keypad) {
    return keypad ? keypad->keycnt : 0;
}

void keypad_write_control(Keypad *keypad, uint16_t val) {
    if (!keypad) return;
    keypad->keycnt = val;
    keypad_check_irq(keypad);
}

void keypad_check_irq(Keypad *keypad) {
    if (!keypad || !keypad->gba) return;
    if (!(keypad->keycnt & (1 << 14))) return; // IRQ disabled

    uint16_t mask = keypad->keycnt & 0x03FF;
    uint16_t pressed = (~keypad->keyinput) & 0x03FF;
    bool cond_and = (keypad->keycnt & (1 << 15)) != 0;

    bool triggered = false;
    if (cond_and) {
        triggered = ((pressed & mask) == mask) && (mask != 0);
    } else {
        triggered = (pressed & mask) != 0;
    }

    if (triggered) {
        interrupt_raise(&keypad->gba->intr, INTR_KEYPAD);
    }
}
