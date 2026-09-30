#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include <stdbool.h>

struct GBA;

typedef struct TimerChannel {
    uint16_t counter;
    uint16_t reload;
    uint16_t cnt;
    uint32_t prescaler_cycles;
    bool enabled;
    bool cascade;
    bool irq_enable;
    int prescaler_shift; // 0 (/1), 6 (/64), 8 (/256), 10 (/1024)
} TimerChannel;

typedef struct Timer {
    TimerChannel ch[4];
    struct GBA *gba;
} Timer;

void timer_init(Timer *timer, struct GBA *gba);
void timer_reset(Timer *timer);
void timer_step(Timer *timer, int cycles);

uint16_t timer_read_counter(Timer *timer, int ch);
uint16_t timer_read_control(Timer *timer, int ch);
void timer_write_reload(Timer *timer, int ch, uint16_t val);
void timer_write_control(Timer *timer, int ch, uint16_t val);

#endif // TIMER_H
