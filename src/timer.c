#include "timer.h"
#include "interrupt.h"
#include "apu.h"
#include "gba.h"
#include <string.h>

/**
 * GBA Hardware Timers (Timers 0 - 3)
 * "Ntutu Advance System"
 */

static const int prescaler_shifts[4] = { 0, 6, 8, 10 }; // 1, 64, 256, 1024

void timer_init(Timer *timer, struct GBA *gba) {
    if (!timer) return;
    memset(timer, 0, sizeof(*timer));
    timer->gba = gba;
    timer_reset(timer);
}

void timer_reset(Timer *timer) {
    if (!timer) return;
    for (int i = 0; i < 4; i++) {
        timer->ch[i].counter = 0;
        timer->ch[i].reload = 0;
        timer->ch[i].cnt = 0;
        timer->ch[i].prescaler_cycles = 0;
        timer->ch[i].enabled = false;
        timer->ch[i].cascade = false;
        timer->ch[i].irq_enable = false;
        timer->ch[i].prescaler_shift = 0;
    }
}

static void timer_overflow(Timer *timer, int i) {
    TimerChannel *ch = &timer->ch[i];
    ch->counter = ch->reload;

    // Raise interrupt if enabled
    if (ch->irq_enable && timer->gba) {
        interrupt_raise(&timer->gba->intr, INTR_TIMER0 << i);
    }

    // DirectSound sample pacing on Timer 0 and Timer 1
    if ((i == 0 || i == 1) && timer->gba) {
        apu_timer_overflow(&timer->gba->apu, i);
    }

    // Cascade mode: increment next timer if cascade enabled
    if (i < 3) {
        TimerChannel *next = &timer->ch[i + 1];
        if (next->enabled && next->cascade) {
            uint32_t val = (uint32_t)next->counter + 1;
            if (val > 0xFFFF) {
                timer_overflow(timer, i + 1);
            } else {
                next->counter = (uint16_t)val;
            }
        }
    }
}

void timer_step(Timer *timer, int cycles) {
    if (!timer) return;

    for (int i = 0; i < 4; i++) {
        TimerChannel *ch = &timer->ch[i];
        if (!ch->enabled || ch->cascade) continue;

        ch->prescaler_cycles += cycles;
        int shift = ch->prescaler_shift;
        uint32_t ticks = ch->prescaler_cycles >> shift;
        if (ticks > 0) {
            ch->prescaler_cycles &= ((1 << shift) - 1);
            uint32_t new_count = (uint32_t)ch->counter + ticks;
            if (new_count > 0xFFFF) {
                timer_overflow(timer, i);
                // Handle excess ticks after reload
                uint32_t excess = new_count - 0x10000;
                ch->counter = (uint16_t)((uint32_t)ch->reload + excess);
            } else {
                ch->counter = (uint16_t)new_count;
            }
        }
    }
}

uint16_t timer_read_counter(Timer *timer, int ch) {
    if (!timer || ch < 0 || ch > 3) return 0;
    return timer->ch[ch].counter;
}

uint16_t timer_read_control(Timer *timer, int ch) {
    if (!timer || ch < 0 || ch > 3) return 0;
    return timer->ch[ch].cnt;
}

void timer_write_reload(Timer *timer, int ch, uint16_t val) {
    if (!timer || ch < 0 || ch > 3) return;
    timer->ch[ch].reload = val;
}

void timer_write_control(Timer *timer, int ch, uint16_t val) {
    if (!timer || ch < 0 || ch > 3) return;
    TimerChannel *t = &timer->ch[ch];
    bool was_enabled = t->enabled;

    t->cnt = val;
    t->prescaler_shift = prescaler_shifts[val & 3];
    t->cascade = (ch > 0) && ((val & 4) != 0);
    t->irq_enable = (val & (1 << 6)) != 0;
    t->enabled = (val & (1 << 7)) != 0;

    // When timer is enabled, reload counter
    if (!was_enabled && t->enabled) {
        t->counter = t->reload;
        t->prescaler_cycles = 0;
    }
}
