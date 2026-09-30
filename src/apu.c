#include "apu.h"
#include "dma.h"
#include "bus.h"
#include "gba.h"
#include <string.h>

/**
 * Game Boy Advance Audio Processing Unit (APU)
 * "Ntutu Advance System"
 */

// Square wave duty cycle waveforms (12.5%, 25%, 50%, 75%)
static const uint8_t duty_waveforms[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 }, // 12.5%
    { 1, 0, 0, 0, 0, 0, 0, 1 }, // 25.0%
    { 1, 0, 0, 0, 0, 1, 1, 1 }, // 50.0%
    { 0, 1, 1, 1, 1, 1, 1, 0 }  // 75.0%
};

void apu_init(APU *apu, struct GBA *gba) {
    if (!apu) return;
    memset(apu, 0, sizeof(*apu));
    apu->gba = gba;
    apu_reset(apu);
}

void apu_reset(APU *apu) {
    if (!apu) return;
    memset(&apu->ch1, 0, sizeof(apu->ch1));
    memset(&apu->ch2, 0, sizeof(apu->ch2));
    memset(&apu->ch3, 0, sizeof(apu->ch3));
    memset(&apu->ch4, 0, sizeof(apu->ch4));
    apu->ch4.lfsr = 0x7FFF;

    memset(&apu->fifo_a, 0, sizeof(apu->fifo_a));
    memset(&apu->fifo_b, 0, sizeof(apu->fifo_b));

    apu->soundcnt_l = 0;
    apu->soundcnt_h = 0;
    apu->soundcnt_x = 0;
    apu->soundbias = 0x0200; // Default bias 512

    apu->dsound_a_timer1 = false;
    apu->dsound_b_timer1 = false;
    apu->dsound_a_left = false;
    apu->dsound_a_right = false;
    apu->dsound_b_left = false;
    apu->dsound_b_right = false;
    apu->dsound_a_volume = 1;
    apu->dsound_b_volume = 1;

    apu->cycle_accumulator = 0;
    apu->frame_sequencer = 0;
    apu->frame_seq_timer = 0;
    apu->audio_buffer_len = 0;
}

void apu_write_fifo(APU *apu, int fifo_id, uint32_t val) {
    FIFO *f = (fifo_id == 0) ? &apu->fifo_a : &apu->fifo_b;
    for (int i = 0; i < 4; i++) {
        if (f->count < 32) {
            f->buffer[f->write_pos] = (int8_t)(val >> (i * 8));
            f->write_pos = (f->write_pos + 1) % 32;
            f->count++;
        }
    }
}

void apu_timer_overflow(APU *apu, int timer_id) {
    if (!apu) return;

    // DirectSound A
    if ((timer_id == 1 && apu->dsound_a_timer1) || (timer_id == 0 && !apu->dsound_a_timer1)) {
        if (apu->fifo_a.count > 0) {
            apu->fifo_a.current_sample = apu->fifo_a.buffer[apu->fifo_a.read_pos];
            apu->fifo_a.read_pos = (apu->fifo_a.read_pos + 1) % 32;
            apu->fifo_a.count--;
        }
        // Request DMA refill if FIFO count <= 16
        if (apu->fifo_a.count <= 16 && apu->gba) {
            dma_trigger(&apu->gba->dma, DMA_TRIGGER_SPECIAL);
        }
    }

    // DirectSound B
    if ((timer_id == 1 && apu->dsound_b_timer1) || (timer_id == 0 && !apu->dsound_b_timer1)) {
        if (apu->fifo_b.count > 0) {
            apu->fifo_b.current_sample = apu->fifo_b.buffer[apu->fifo_b.read_pos];
            apu->fifo_b.read_pos = (apu->fifo_b.read_pos + 1) % 32;
            apu->fifo_b.count--;
        }
        if (apu->fifo_b.count <= 16 && apu->gba) {
            dma_trigger(&apu->gba->dma, DMA_TRIGGER_SPECIAL);
        }
    }
}

static void apu_mix_sample(APU *apu) {
    // DMG Channels synthesis
    int ch1_out = 0;
    if (apu->ch1.enabled) {
        int duty = (apu->ch1.duty_length >> 6) & 3;
        if (duty_waveforms[duty][apu->ch1.duty_step]) {
            ch1_out = apu->ch1.volume;
        }
    }

    int ch2_out = 0;
    if (apu->ch2.enabled) {
        int duty = (apu->ch2.duty_length >> 6) & 3;
        if (duty_waveforms[duty][apu->ch2.duty_step]) {
            ch2_out = apu->ch2.volume;
        }
    }

    int ch3_out = 0;
    if (apu->ch3.enabled) {
        uint8_t byte = apu->ch3.wave_ram[apu->ch3.sample_index / 2];
        uint8_t sample = (apu->ch3.sample_index & 1) ? (byte & 0x0F) : (byte >> 4);
        ch3_out = sample >> apu->ch3.volume_shift;
    }

    int ch4_out = 0;
    if (apu->ch4.enabled) {
        if (!(apu->ch4.lfsr & 1)) {
            ch4_out = apu->ch4.volume;
        }
    }

    // DirectSound channels
    int dsound_a = (int)apu->fifo_a.current_sample * apu->dsound_a_volume;
    int dsound_b = (int)apu->fifo_b.current_sample * apu->dsound_b_volume;

    // Mix stereo output
    int left = 0;
    int right = 0;

    // DMG channel routing from SOUNDCNT_L
    if (apu->soundcnt_l & (1 << 8))  left += ch1_out * 64;
    if (apu->soundcnt_l & (1 << 9))  left += ch2_out * 64;
    if (apu->soundcnt_l & (1 << 10)) left += ch3_out * 64;
    if (apu->soundcnt_l & (1 << 11)) left += ch4_out * 64;

    if (apu->soundcnt_l & (1 << 12)) right += ch1_out * 64;
    if (apu->soundcnt_l & (1 << 13)) right += ch2_out * 64;
    if (apu->soundcnt_l & (1 << 14)) right += ch3_out * 64;
    if (apu->soundcnt_l & (1 << 15)) right += ch4_out * 64;

    // DirectSound routing from SOUNDCNT_H
    if (apu->dsound_a_left)  left += dsound_a * 8;
    if (apu->dsound_a_right) right += dsound_a * 8;
    if (apu->dsound_b_left)  left += dsound_b * 8;
    if (apu->dsound_b_right) right += dsound_b * 8;

    // Clamp 16-bit
    if (left > 32767) left = 32767;
    if (left < -32768) left = -32768;
    if (right > 32767) right = 32767;
    if (right < -32768) right = -32768;

    // Push into ring buffer
    if (apu->audio_buffer_len < APU_BUFFER_SIZE) {
        apu->audio_buffer[apu->audio_buffer_len * 2 + 0] = (int16_t)left;
        apu->audio_buffer[apu->audio_buffer_len * 2 + 1] = (int16_t)right;
        apu->audio_buffer_len++;
    }
}

void apu_step(APU *apu, int cycles) {
    if (!apu) return;

    // 16,777,216 Hz / 44,100 Hz ≈ 380.435 cycles per sample
    apu->cycle_accumulator += cycles;
    while (apu->cycle_accumulator >= 380) {
        apu->cycle_accumulator -= 380;
        apu_mix_sample(apu);
    }

    // Step DMG Channel 1 frequency timer
    if (apu->ch1.enabled) {
        apu->ch1.timer -= cycles;
        if (apu->ch1.timer <= 0) {
            int freq = apu->ch1.freq & 0x7FF;
            apu->ch1.timer += (2048 - freq) * 4;
            apu->ch1.duty_step = (apu->ch1.duty_step + 1) % 8;
        }
    }

    // Step DMG Channel 2 frequency timer
    if (apu->ch2.enabled) {
        apu->ch2.timer -= cycles;
        if (apu->ch2.timer <= 0) {
            int freq = apu->ch2.freq & 0x7FF;
            apu->ch2.timer += (2048 - freq) * 4;
            apu->ch2.duty_step = (apu->ch2.duty_step + 1) % 8;
        }
    }
}

uint16_t apu_read_reg(APU *apu, uint32_t addr) {
    if (!apu) return 0;
    switch (addr) {
        case 0x60: return apu->ch1.sweep;
        case 0x62: return apu->ch1.duty_length;
        case 0x64: return apu->ch1.freq;
        case 0x68: return apu->ch2.duty_length;
        case 0x6C: return apu->ch2.freq;
        case 0x70: return apu->ch3.control;
        case 0x72: return apu->ch3.length;
        case 0x74: return apu->ch3.freq;
        case 0x78: return apu->ch4.length;
        case 0x7C: return apu->ch4.freq;
        case 0x80: return apu->soundcnt_l;
        case 0x82: return apu->soundcnt_h;
        case 0x84: return apu->soundcnt_x;
        case 0x88: return apu->soundbias;
        default:   return 0;
    }
}

void apu_write_reg(APU *apu, uint32_t addr, uint16_t val) {
    if (!apu) return;
    switch (addr) {
        case 0x60: apu->ch1.sweep = val; break;
        case 0x62:
            apu->ch1.duty_length = val;
            apu->ch1.volume = (val >> 12) & 0xF;
            break;
        case 0x64:
            apu->ch1.freq = val;
            if (val & (1 << 15)) {
                apu->ch1.enabled = true;
                apu->ch1.volume = (apu->ch1.duty_length >> 12) & 0xF;
                apu->ch1.timer = (2048 - (val & 0x7FF)) * 4;
            }
            break;

        case 0x68:
            apu->ch2.duty_length = val;
            apu->ch2.volume = (val >> 12) & 0xF;
            break;
        case 0x6C:
            apu->ch2.freq = val;
            if (val & (1 << 15)) {
                apu->ch2.enabled = true;
                apu->ch2.volume = (apu->ch2.duty_length >> 12) & 0xF;
                apu->ch2.timer = (2048 - (val & 0x7FF)) * 4;
            }
            break;

        case 0x70: apu->ch3.control = val; break;
        case 0x72: apu->ch3.length = val; break;
        case 0x74:
            apu->ch3.freq = val;
            if (val & (1 << 15)) {
                apu->ch3.enabled = true;
                apu->ch3.sample_index = 0;
            }
            break;

        case 0x78:
            apu->ch4.length = val;
            apu->ch4.volume = (val >> 12) & 0xF;
            break;
        case 0x7C:
            apu->ch4.freq = val;
            if (val & (1 << 15)) {
                apu->ch4.enabled = true;
                apu->ch4.volume = (apu->ch4.length >> 12) & 0xF;
                apu->ch4.lfsr = 0x7FFF;
            }
            break;

        case 0x80: apu->soundcnt_l = val; break;
        case 0x82:
            apu->soundcnt_h = val;
            apu->dsound_a_volume = (val & (1 << 2)) ? 1 : 0; // 100% or 50%
            apu->dsound_b_volume = (val & (1 << 3)) ? 1 : 0;
            apu->dsound_a_right = (val & (1 << 8)) != 0;
            apu->dsound_a_left  = (val & (1 << 9)) != 0;
            apu->dsound_a_timer1= (val & (1 << 10)) != 0;
            apu->dsound_b_right = (val & (1 << 12)) != 0;
            apu->dsound_b_left  = (val & (1 << 13)) != 0;
            apu->dsound_b_timer1= (val & (1 << 14)) != 0;
            if (val & (1 << 11)) { apu->fifo_a.count = 0; apu->fifo_a.read_pos = apu->fifo_a.write_pos = 0; }
            if (val & (1 << 15)) { apu->fifo_b.count = 0; apu->fifo_b.read_pos = apu->fifo_b.write_pos = 0; }
            break;

        case 0x84: apu->soundcnt_x = val; break;
        case 0x88: apu->soundbias = val; break;

        case 0xA0: apu_write_fifo(apu, 0, (uint32_t)val); break;
        case 0xA2: apu_write_fifo(apu, 0, (uint32_t)val << 16); break;
        case 0xA4: apu_write_fifo(apu, 1, (uint32_t)val); break;
        case 0xA6: apu_write_fifo(apu, 1, (uint32_t)val << 16); break;
    }
}

size_t apu_read_samples(APU *apu, int16_t *out_buffer, size_t max_samples) {
    if (!apu || !out_buffer) return 0;
    size_t count = apu->audio_buffer_len;
    if (count > max_samples) count = max_samples;
    if (count > 0) {
        memcpy(out_buffer, apu->audio_buffer, count * 2 * sizeof(int16_t));
        // Shift remaining samples
        if (apu->audio_buffer_len > count) {
            memmove(apu->audio_buffer, &apu->audio_buffer[count * 2], (apu->audio_buffer_len - count) * 2 * sizeof(int16_t));
        }
        apu->audio_buffer_len -= count;
    }
    return count;
}
