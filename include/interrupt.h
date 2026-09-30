#ifndef INTERRUPT_H
#define INTERRUPT_H

#include <stdint.h>
#include <stdbool.h>

struct GBA;

/* Interrupt Request / Enable Bits */
#define INTR_VBLANK   (1 << 0)
#define INTR_HBLANK   (1 << 1)
#define INTR_VCOUNT   (1 << 2)
#define INTR_TIMER0   (1 << 3)
#define INTR_TIMER1   (1 << 4)
#define INTR_TIMER2   (1 << 5)
#define INTR_TIMER3   (1 << 6)
#define INTR_SERIAL   (1 << 7)
#define INTR_DMA0     (1 << 8)
#define INTR_DMA1     (1 << 9)
#define INTR_DMA2     (1 << 10)
#define INTR_DMA3     (1 << 11)
#define INTR_KEYPAD   (1 << 12)
#define INTR_GAMEPAK  (1 << 13)

typedef struct Interrupt {
    uint16_t ime; // Interrupt Master Enable (Bit 0)
    uint16_t ie;  // Interrupt Enable Mask
    uint16_t if_; // Interrupt Request Flags
    struct GBA *gba;
} Interrupt;

void interrupt_init(Interrupt *intr, struct GBA *gba);
void interrupt_reset(Interrupt *intr);
void interrupt_raise(Interrupt *intr, uint16_t mask);
bool interrupt_check_pending(Interrupt *intr);
void interrupt_dispatch(Interrupt *intr);

#endif // INTERRUPT_H
