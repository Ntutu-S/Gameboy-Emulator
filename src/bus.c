#include "bus.h"
#include "gba.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Game Boy Advance Memory Bus Subsystem ("Ntutu Advance System")
 */

void bus_init(GBABus *bus, struct GBA *gba) {
    if (!bus) return;
    memset(bus, 0, sizeof(*bus));
    bus->gba = gba;
    bus->open_bus = 0x00000000;
    bus->eeprom.size = 8192; // Default to 64Kbit EEPROM
}

bool bus_load_rom(GBABus *bus, const char *rom_path) {
    if (!bus || !rom_path) return false;

    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "[BUS] Failed to open ROM: %s\n", rom_path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return false;
    }

    bus->rom = (uint8_t *)malloc(size);
    if (!bus->rom) {
        fclose(f);
        return false;
    }

    if (fread(bus->rom, 1, size, f) != (size_t)size) {
        free(bus->rom);
        bus->rom = NULL;
        fclose(f);
        return false;
    }
    fclose(f);

    bus->rom_size = (size_t)size;

    // Extract ROM title (12 bytes at 0xA0)
    memset(bus->rom_title, 0, sizeof(bus->rom_title));
    if (bus->rom_size >= 0xAC) {
        memcpy(bus->rom_title, &bus->rom[0xA0], 12);
        for (int i = 0; i < 12; i++) {
            if (bus->rom_title[i] < 32 || bus->rom_title[i] > 126) {
                bus->rom_title[i] = '\0';
                break;
            }
        }
    }
    if (bus->rom_title[0] == '\0') {
        strncpy(bus->rom_title, "GBA Game", sizeof(bus->rom_title) - 1);
    }

    // Prepare save path: <rom_path>.sav
    snprintf(bus->save_path, sizeof(bus->save_path), "%s.sav", rom_path);
    bus_load_sram(bus);

    printf("[BUS] Loaded ROM \"%s\" (%zu KB), Title: %s\n", rom_path, bus->rom_size / 1024, bus->rom_title);
    return true;
}

void bus_load_sram(GBABus *bus) {
    if (!bus || bus->save_path[0] == '\0') return;
    FILE *f = fopen(bus->save_path, "rb");
    if (f) {
        size_t n = fread(bus->sram, 1, sizeof(bus->sram), f);
        (void)n;
        fclose(f);
        printf("[BUS] Loaded save data from %s\n", bus->save_path);
    }
}

void bus_flush_sram(GBABus *bus) {
    if (!bus || !bus->sram_dirty || bus->save_path[0] == '\0') return;
    FILE *f = fopen(bus->save_path, "wb");
    if (f) {
        fwrite(bus->sram, 1, sizeof(bus->sram), f);
        fclose(f);
        bus->sram_dirty = false;
        printf("[BUS] Flushed save data to %s\n", bus->save_path);
    }
}

void bus_cleanup(GBABus *bus) {
    if (!bus) return;
    bus_flush_sram(bus);
    if (bus->rom) {
        free(bus->rom);
        bus->rom = NULL;
    }
}

// ----------------------------------------------------------------------------
// Serial EEPROM emulation over DMA3
// ----------------------------------------------------------------------------
uint16_t eeprom_read(GBABus *bus) {
    EEPROM *eep = &bus->eeprom;
    if (eep->state == EEPROM_READ_DATA) {
        if (eep->read_pos < 68) {
            uint8_t bit = eep->read_stream[eep->read_pos++];
            if (eep->read_pos >= 68) {
                eep->state = EEPROM_IDLE;
            }
            return bit & 1;
        }
    }
    // Polling reads return bit 0 = 1 (ready)
    return 1;
}

void eeprom_write(GBABus *bus, uint16_t val, int count) {
    EEPROM *eep = &bus->eeprom;
    uint8_t bit = val & 1;

    // Detect address length on first command
    if (eep->state == EEPROM_IDLE) {
        eep->state = EEPROM_COMMAND;
        eep->bits_received = 0;
        eep->buffer = 0;
        eep->addr_len = (count > 9) ? 14 : 6;
        eep->size = (count > 9) ? 8192 : 512;
    }

    if (eep->state == EEPROM_COMMAND) {
        eep->buffer = (eep->buffer << 1) | bit;
        eep->bits_received++;
        if (eep->bits_received == 2) {
            eep->rw_mode = eep->buffer & 3; // 2 = write, 3 = read
            eep->state = EEPROM_ADDRESS;
            eep->bits_received = 0;
            eep->buffer = 0;
        }
        return;
    }

    if (eep->state == EEPROM_ADDRESS) {
        eep->buffer = (eep->buffer << 1) | bit;
        eep->bits_received++;
        if (eep->bits_received == eep->addr_len) {
            eep->address = (uint16_t)eep->buffer;
            eep->bits_received = 0;
            eep->buffer = 0;
            if (eep->rw_mode == 3) {
                // Read request: Prepare 68-halfword response stream
                // (4 dummy zero bits + 64 data bits)
                memset(eep->read_stream, 0, sizeof(eep->read_stream));
                uint32_t byte_addr = (uint32_t)eep->address * 8;
                for (int i = 0; i < 64; i++) {
                    int byte_idx = byte_addr + (i / 8);
                    int bit_idx = 7 - (i % 8);
                    uint8_t b = (byte_idx < 8192) ? eep->data[byte_idx] : 0;
                    eep->read_stream[4 + i] = (b >> bit_idx) & 1;
                }
                eep->read_pos = 0;
                eep->state = EEPROM_READ_DATA;
            } else {
                eep->state = EEPROM_WRITE_DATA;
            }
        }
        return;
    }

    if (eep->state == EEPROM_WRITE_DATA) {
        eep->buffer = (eep->buffer << 1) | bit;
        eep->bits_received++;
        if ((eep->bits_received % 8) == 0) {
            uint32_t byte_addr = (uint32_t)eep->address * 8 + (eep->bits_received / 8 - 1);
            if (byte_addr < 8192) {
                eep->data[byte_addr] = (uint8_t)eep->buffer;
                eep->dirty = true;
            }
        }
        if (eep->bits_received == 64) {
            eep->state = EEPROM_IDLE;
        }
    }
}

// ----------------------------------------------------------------------------
// I/O Registers dispatch
// ----------------------------------------------------------------------------
uint8_t bus_read_io8(GBABus *bus, uint32_t addr) {
    uint32_t reg = addr & 0x3FE;
    uint16_t val = bus_read_io16(bus, reg);
    return (addr & 1) ? (uint8_t)(val >> 8) : (uint8_t)val;
}

uint16_t bus_read_io16(GBABus *bus, uint32_t addr) {
    GBA *gba = bus->gba;
    if (!gba) return 0;
    uint32_t offset = addr & 0x3FE;

    // PPU Registers: 0x04000000 - 0x0400005F
    if (offset < 0x060) {
        return ppu_read_reg(&gba->ppu, offset);
    }
    // APU Registers: 0x04000060 - 0x040000AF
    if (offset >= 0x060 && offset < 0x0B0) {
        return apu_read_reg(&gba->apu, offset);
    }
    // DMA Registers: 0x040000B0 - 0x040000DF
    if (offset >= 0x0B0 && offset <= 0x0DF) {
        int ch = (offset - 0x0B0) / 12;
        int reg = (offset - 0x0B0) % 12;
        if (reg == 10) return dma_read_cnt_h(&gba->dma, ch);
        return *(uint16_t*)&bus->io[offset];
    }
    // Timer Registers: 0x04000100 - 0x0400010F
    if (offset >= 0x100 && offset <= 0x10E) {
        int ch = (offset - 0x100) / 4;
        bool is_cnt = (offset & 2) != 0;
        return is_cnt ? timer_read_control(&gba->timer, ch) : timer_read_counter(&gba->timer, ch);
    }
    // Keypad Registers: 0x04000130, 0x04000132
    if (offset == 0x130) return keypad_read_input(&gba->keypad);
    if (offset == 0x132) return keypad_read_control(&gba->keypad);

    // Interrupt Registers: 0x04000200 - 0x04000208
    if (offset == 0x200) return gba->intr.ie;
    if (offset == 0x202) return gba->intr.if_;
    if (offset == 0x208) return gba->intr.ime;

    return *(uint16_t*)&bus->io[offset];
}

uint32_t bus_read_io32(GBABus *bus, uint32_t addr) {
    uint16_t lo = bus_read_io16(bus, addr & ~3u);
    uint16_t hi = bus_read_io16(bus, (addr & ~3u) + 2);
    return (uint32_t)lo | ((uint32_t)hi << 16);
}

void bus_write_io8(GBABus *bus, uint32_t addr, uint8_t val) {
    uint32_t reg = addr & 0x3FE;
    uint16_t old = bus_read_io16(bus, reg);
    if (addr & 1) {
        bus_write_io16(bus, reg, (old & 0x00FF) | ((uint16_t)val << 8));
    } else {
        bus_write_io16(bus, reg, (old & 0xFF00) | val);
    }
}

void bus_write_io16(GBABus *bus, uint32_t addr, uint16_t val) {
    GBA *gba = bus->gba;
    if (!gba) return;
    uint32_t offset = addr & 0x3FE;
    *(uint16_t*)&bus->io[offset] = val;

    // PPU Registers
    if (offset < 0x060) {
        ppu_write_reg(&gba->ppu, offset, val);
        return;
    }
    // APU Registers
    if (offset >= 0x060 && offset < 0x0B0) {
        apu_write_reg(&gba->apu, offset, val);
        return;
    }
    // DMA Registers
    if (offset >= 0x0B0 && offset <= 0x0DF) {
        int ch = (offset - 0x0B0) / 12;
        int reg = (offset - 0x0B0) % 12;
        if (reg == 0) dma_write_src(&gba->dma, ch, val, 0);
        else if (reg == 2) dma_write_src(&gba->dma, ch, (uint32_t)val << 16, 2);
        else if (reg == 4) dma_write_dst(&gba->dma, ch, val, 0);
        else if (reg == 6) dma_write_dst(&gba->dma, ch, (uint32_t)val << 16, 2);
        else if (reg == 8) dma_write_count(&gba->dma, ch, val);
        else if (reg == 10) dma_write_cnt_h(&gba->dma, ch, val);
        return;
    }
    // Timer Registers
    if (offset >= 0x100 && offset <= 0x10E) {
        int ch = (offset - 0x100) / 4;
        bool is_cnt = (offset & 2) != 0;
        if (is_cnt) timer_write_control(&gba->timer, ch, val);
        else timer_write_reload(&gba->timer, ch, val);
        return;
    }
    // Keypad
    if (offset == 0x132) {
        keypad_write_control(&gba->keypad, val);
        return;
    }
    // Interrupt Registers
    if (offset == 0x200) { gba->intr.ie = val; return; }
    if (offset == 0x202) { gba->intr.if_ &= ~val; return; } // Write 1 to clear
    if (offset == 0x208) { gba->intr.ime = val & 1; return; }
}

void bus_write_io32(GBABus *bus, uint32_t addr, uint32_t val) {
    bus_write_io16(bus, addr & ~3u, (uint16_t)val);
    bus_write_io16(bus, (addr & ~3u) + 2, (uint16_t)(val >> 16));
}

// ----------------------------------------------------------------------------
// 8-bit, 16-bit, and 32-bit Memory Accessors with Hardware Bus Quirks
// ----------------------------------------------------------------------------
uint8_t bus_read8(GBABus *bus, uint32_t addr) {
    uint32_t page = addr >> 24;

    switch (page) {
        case 0x00: // BIOS (16 KB)
            // BIOS Open-Bus Protection: reading BIOS when PC >= 0x4000 returns open_bus
            if (bus->gba && bus->gba->cpu.r[REG_PC] >= 0x4000) {
                return (uint8_t)(bus->open_bus >> ((addr & 3) * 8));
            }
            if (addr < 0x4000) return bus->bios[addr];
            return (uint8_t)(bus->open_bus >> ((addr & 3) * 8));

        case 0x02: // EWRAM (256 KB, mirrored)
            return bus->ewram[addr & 0x0003FFFF];

        case 0x03: // IWRAM (32 KB, mirrored)
            return bus->iwram[addr & 0x00007FFF];

        case 0x04: // I/O Registers
            if ((addr & 0x00FFFFFF) < 0x400) {
                return bus_read_io8(bus, addr);
            }
            return (uint8_t)(bus->open_bus >> ((addr & 3) * 8));

        case 0x05: // Palette RAM (1 KB, mirrored)
            return bus->palette[addr & 0x000003FF];

        case 0x06: // VRAM (96 KB, mirrored)
            {
                uint32_t offset = addr & 0x0001FFFF;
                if (offset >= 0x18000) offset = 0x10000 + (offset & 0x7FFF);
                return bus->vram[offset];
            }

        case 0x07: // OAM (1 KB, mirrored)
            return bus->oam[addr & 0x000003FF];

        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D: // Game Pak ROM Waitstates 0, 1, 2
            if (page == 0x0D) {
                // EEPROM reads
                return (uint8_t)eeprom_read(bus);
            }
            if (bus->rom && bus->rom_size > 0) {
                uint32_t rom_offset = (addr & 0x01FFFFFF) % bus->rom_size;
                return bus->rom[rom_offset];
            }
            return (uint8_t)(bus->open_bus >> ((addr & 3) * 8));

        case 0x0E:
        case 0x0F: // SRAM / Flash Backup Memory
            return bus->sram[addr & 0x0000FFFF];

        default:
            return (uint8_t)(bus->open_bus >> ((addr & 3) * 8));
    }
}

uint16_t bus_read16(GBABus *bus, uint32_t addr) {
    // Unaligned LDRH hardware quirk: if bit 0 set, read aligned halfword and rotate right by 8
    if (addr & 1) {
        uint16_t val = bus_read16(bus, addr & ~1u);
        return (val >> 8) | (val << 8);
    }

    uint32_t page = addr >> 24;

    switch (page) {
        case 0x00:
            if (bus->gba && bus->gba->cpu.r[REG_PC] >= 0x4000) {
                return (uint16_t)(bus->open_bus >> ((addr & 2) * 8));
            }
            if (addr < 0x4000) return *(uint16_t*)&bus->bios[addr];
            return (uint16_t)(bus->open_bus >> ((addr & 2) * 8));

        case 0x02:
            return *(uint16_t*)&bus->ewram[addr & 0x0003FFFE];

        case 0x03:
            return *(uint16_t*)&bus->iwram[addr & 0x00007FFE];

        case 0x04:
            if ((addr & 0x00FFFFFF) < 0x400) {
                return bus_read_io16(bus, addr);
            }
            return (uint16_t)(bus->open_bus >> ((addr & 2) * 8));

        case 0x05:
            return *(uint16_t*)&bus->palette[addr & 0x000003FE];

        case 0x06:
            {
                uint32_t offset = addr & 0x0001FFFE;
                if (offset >= 0x18000) offset = 0x10000 + (offset & 0x7FFF);
                return *(uint16_t*)&bus->vram[offset];
            }

        case 0x07:
            return *(uint16_t*)&bus->oam[addr & 0x000003FE];

        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
            if (page == 0x0D) {
                return eeprom_read(bus);
            }
            if (bus->rom && bus->rom_size > 0) {
                uint32_t rom_offset = (addr & 0x01FFFFFF) % bus->rom_size;
                return *(uint16_t*)&bus->rom[rom_offset & ~1u];
            }
            return (uint16_t)(bus->open_bus >> ((addr & 2) * 8));

        case 0x0E:
        case 0x0F:
            {
                uint8_t byte = bus->sram[addr & 0x0000FFFF];
                return (uint16_t)byte | ((uint16_t)byte << 8);
            }

        default:
            return (uint16_t)(bus->open_bus >> ((addr & 2) * 8));
    }
}

uint32_t bus_read32(GBABus *bus, uint32_t addr) {
    // Unaligned LDR hardware quirk: read aligned word and rotate right by (addr & 3) * 8
    uint32_t rot = (addr & 3) * 8;
    addr &= ~3u;

    uint32_t val;
    uint32_t page = addr >> 24;

    switch (page) {
        case 0x00:
            if (bus->gba && bus->gba->cpu.r[REG_PC] >= 0x4000) {
                val = bus->open_bus;
            } else if (addr < 0x4000) {
                val = *(uint32_t*)&bus->bios[addr];
            } else {
                val = bus->open_bus;
            }
            break;

        case 0x02:
            val = *(uint32_t*)&bus->ewram[addr & 0x0003FFFC];
            break;

        case 0x03:
            val = *(uint32_t*)&bus->iwram[addr & 0x00007FFC];
            break;

        case 0x04:
            if ((addr & 0x00FFFFFF) < 0x400) {
                val = bus_read_io32(bus, addr);
            } else {
                val = bus->open_bus;
            }
            break;

        case 0x05:
            val = *(uint32_t*)&bus->palette[addr & 0x000003FC];
            break;

        case 0x06:
            {
                uint32_t offset = addr & 0x0001FFFC;
                if (offset >= 0x18000) offset = 0x10000 + (offset & 0x7FFF);
                val = *(uint32_t*)&bus->vram[offset];
            }
            break;

        case 0x07:
            val = *(uint32_t*)&bus->oam[addr & 0x000003FC];
            break;

        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
            if (page == 0x0D) {
                val = eeprom_read(bus);
            } else if (bus->rom && bus->rom_size > 0) {
                uint32_t rom_offset = (addr & 0x01FFFFFF) % bus->rom_size;
                val = *(uint32_t*)&bus->rom[rom_offset & ~3u];
            } else {
                val = bus->open_bus;
            }
            break;

        case 0x0E:
        case 0x0F:
            {
                uint8_t byte = bus->sram[addr & 0x0000FFFF];
                val = (uint32_t)byte | ((uint32_t)byte << 8) | ((uint32_t)byte << 16) | ((uint32_t)byte << 24);
            }
            break;

        default:
            val = bus->open_bus;
            break;
    }

    bus->open_bus = val;

    if (rot != 0) {
        return (val >> rot) | (val << (32 - rot));
    }
    return val;
}

void bus_write8(GBABus *bus, uint32_t addr, uint8_t val) {
    uint32_t page = addr >> 24;

    switch (page) {
        case 0x02: // EWRAM
            bus->ewram[addr & 0x0003FFFF] = val;
            break;

        case 0x03: // IWRAM
            bus->iwram[addr & 0x00007FFF] = val;
            break;

        case 0x04: // I/O
            if ((addr & 0x00FFFFFF) < 0x400) {
                bus_write_io8(bus, addr, val);
            }
            break;

        case 0x05: // Palette 8-bit Write Quirk: Expand byte across 16-bit halfword
            {
                uint16_t expanded = ((uint16_t)val << 8) | val;
                *(uint16_t*)&bus->palette[addr & 0x000003FE] = expanded;
            }
            break;

        case 0x06: // VRAM 8-bit Write Quirk:
            {
                uint32_t offset = addr & 0x0001FFFF;
                if (offset >= 0x18000) offset = 0x10000 + (offset & 0x7FFF);
                // Background VRAM (< 0x10000 / < 0x06010000) expands byte; OBJ VRAM (>= 0x10000) ignores 8-bit writes
                if (offset < 0x10000) {
                    uint16_t expanded = ((uint16_t)val << 8) | val;
                    *(uint16_t*)&bus->vram[offset & ~1u] = expanded;
                }
            }
            break;

        case 0x07: // OAM 8-bit Write Quirk: Completely ignored by hardware
            break;

        case 0x0D: // EEPROM
            eeprom_write(bus, val, 1);
            break;

        case 0x0E:
        case 0x0F: // SRAM
            bus->sram[addr & 0x0000FFFF] = val;
            bus->sram_dirty = true;
            break;

        default:
            break;
    }
}

void bus_write16(GBABus *bus, uint32_t addr, uint16_t val) {
    addr &= ~1u; // Mask alignment bit
    uint32_t page = addr >> 24;

    switch (page) {
        case 0x02:
            *(uint16_t*)&bus->ewram[addr & 0x0003FFFE] = val;
            break;

        case 0x03:
            *(uint16_t*)&bus->iwram[addr & 0x00007FFE] = val;
            break;

        case 0x04:
            if ((addr & 0x00FFFFFF) < 0x400) {
                bus_write_io16(bus, addr, val);
            }
            break;

        case 0x05:
            *(uint16_t*)&bus->palette[addr & 0x000003FE] = val;
            break;

        case 0x06:
            {
                uint32_t offset = addr & 0x0001FFFE;
                if (offset >= 0x18000) offset = 0x10000 + (offset & 0x7FFF);
                *(uint16_t*)&bus->vram[offset] = val;
            }
            break;

        case 0x07:
            *(uint16_t*)&bus->oam[addr & 0x000003FE] = val;
            break;

        case 0x0D:
            eeprom_write(bus, val, 1);
            break;

        case 0x0E:
        case 0x0F:
            bus->sram[addr & 0x0000FFFF] = (uint8_t)val;
            bus->sram_dirty = true;
            break;

        default:
            break;
    }
}

void bus_write32(GBABus *bus, uint32_t addr, uint32_t val) {
    addr &= ~3u; // Mask alignment bits
    uint32_t page = addr >> 24;

    switch (page) {
        case 0x02:
            *(uint32_t*)&bus->ewram[addr & 0x0003FFFC] = val;
            break;

        case 0x03:
            *(uint32_t*)&bus->iwram[addr & 0x00007FFC] = val;
            break;

        case 0x04:
            if ((addr & 0x00FFFFFF) < 0x400) {
                bus_write_io32(bus, addr, val);
            }
            break;

        case 0x05:
            *(uint32_t*)&bus->palette[addr & 0x000003FC] = val;
            break;

        case 0x06:
            {
                uint32_t offset = addr & 0x0001FFFC;
                if (offset >= 0x18000) offset = 0x10000 + (offset & 0x7FFF);
                *(uint32_t*)&bus->vram[offset] = val;
            }
            break;

        case 0x07:
            *(uint32_t*)&bus->oam[addr & 0x000003FC] = val;
            break;

        case 0x0D:
            eeprom_write(bus, (uint16_t)val, 1);
            break;

        case 0x0E:
        case 0x0F:
            bus->sram[addr & 0x0000FFFF] = (uint8_t)val;
            bus->sram_dirty = true;
            break;

        default:
            break;
    }
}
