#ifndef APU_H
#define APU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct GBA;

#define AUDIO_SAMPLE_RATE 44100
#define APU_BUFFER_SIZE   8192

// DirectSound FIFO buffer (32 bytes)
typedef struct FIFO {
    int8_t buffer[32];
    int read_pos;
    int write_pos;
    int count;
    int8_t current_sample;
} FIFO;

typedef struct DMGChannel1 {
    uint16_t sweep;
    uint16_t duty_length;
    uint16_t env;
    uint16_t freq;
    bool enabled;
    int length_counter;
    int duty_step;
    int timer;
    int volume;
    int env_counter;
    int sweep_counter;
    int sweep_freq;
} DMGChannel1;

typedef struct DMGChannel2 {
    uint16_t duty_length;
    uint16_t env;
    uint16_t freq;
    bool enabled;
    int length_counter;
    int duty_step;
    int timer;
    int volume;
    int env_counter;
} DMGChannel2;

typedef struct DMGChannel3 {
    uint16_t control;
    uint16_t length;
    uint16_t freq;
    uint8_t wave_ram[16]; // 32 4-bit samples
    bool enabled;
    int length_counter;
    int timer;
    int sample_index;
    int volume_shift;
} DMGChannel3;

typedef struct DMGChannel4 {
    uint16_t length;
    uint16_t env;
    uint16_t freq;
    bool enabled;
    int length_counter;
    int timer;
    int volume;
    int env_counter;
    uint16_t lfsr;
    bool width_7bit;
} DMGChannel4;

typedef struct APU {
    DMGChannel1 ch1;
    DMGChannel2 ch2;
    DMGChannel3 ch3;
    DMGChannel4 ch4;

    FIFO fifo_a;
    FIFO fifo_b;

    // Registers
    uint16_t soundcnt_l;
    uint16_t soundcnt_h;
    uint16_t soundcnt_x;
    uint16_t soundbias;

    // DirectSound configuration
    bool dsound_a_timer1; // false = timer 0, true = timer 1
    bool dsound_b_timer1;
    bool dsound_a_left;
    bool dsound_a_right;
    bool dsound_b_left;
    bool dsound_b_right;
    int dsound_a_volume;  // 50% or 100%
    int dsound_b_volume;

    // Sample generation timing
    int cycle_accumulator;
    int frame_sequencer;
    int frame_seq_timer;

    // Output audio buffer (interleaved stereo 16-bit PCM)
    int16_t audio_buffer[APU_BUFFER_SIZE * 2];
    size_t audio_buffer_len; // in sample frames

    struct GBA *gba;
} APU;

void apu_init(APU *apu, struct GBA *gba);
void apu_reset(APU *apu);
void apu_step(APU *apu, int cycles);
void apu_timer_overflow(APU *apu, int timer_id);
void apu_write_fifo(APU *apu, int fifo_id, uint32_t val);

uint16_t apu_read_reg(APU *apu, uint32_t addr);
void apu_write_reg(APU *apu, uint32_t addr, uint16_t val);

size_t apu_read_samples(APU *apu, int16_t *out_buffer, size_t max_samples);

#endif // APU_H
