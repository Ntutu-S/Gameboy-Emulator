#ifndef KEYPAD_H
#define KEYPAD_H

#include <stdint.h>
#include <stdbool.h>

struct GBA;

#define KEY_A      0
#define KEY_B      1
#define KEY_SELECT 2
#define KEY_START  3
#define KEY_RIGHT  4
#define KEY_LEFT   5
#define KEY_UP     6
#define KEY_DOWN   7
#define KEY_R      8
#define KEY_L      9

typedef struct Keypad {
    uint16_t keyinput; // Active low (0 = pressed, 1 = released)
    uint16_t keycnt;   // Interrupt control
    struct GBA *gba;
} Keypad;

void keypad_init(Keypad *keypad, struct GBA *gba);
void keypad_reset(Keypad *keypad);
void keypad_set_key(Keypad *keypad, int key, bool pressed);
uint16_t keypad_read_input(const Keypad *keypad);
uint16_t keypad_read_control(const Keypad *keypad);
void keypad_write_control(Keypad *keypad, uint16_t val);
void keypad_check_irq(Keypad *keypad);

#endif // KEYPAD_H
