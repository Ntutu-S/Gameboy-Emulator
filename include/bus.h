#ifndef BUS_H
#define BUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct GBA;

/**
 * Game Boy Advance 32-bit Memory Bus ("Ntutu Advance System")
 */

typedef enum EEPROMState {
    EEPROM_IDLE,
    EEPROM_COMMAND,
    EEPROM_ADDRESS,
    EEPROM_READ_DUMMY,
    EEPROM_READ_DATA,
    EEPROM_WRITE_DATA
} EEPROMState;

typedef struct EEPROM {
    uint8_t data[8192];     // Supports up to 64Kbit (8KB) or 4Kbit (512 bytes)
    size_t size;            // 512 or 8192 bytes
    EEPROMState state;
    uint32_t buffer;
    int bits_received;
    uint16_t address;
    int addr_len;           // 6 bits for 4Kbit, 14 bits for 64Kbit
    int rw_mode;            // 2 = write, 3 = read
    uint8_t read_stream[68];// 4 dummy zero bits + 64 data bits
    int read_pos;
    bool dirty;
} EEPROM;

typedef struct GBABus {
    uint8_t bios[0x4000];       // 16 KB BIOS
    uint8_t ewram[0x40000];     // 256 KB On-Board Work RAM
    uint8_t iwram[0x8000];      // 32 KB On-Chip Work RAM
    uint8_t io[0x400];          // 1 KB I/O Registers
    uint8_t palette[0x400];     // 1 KB Palette RAM
    uint8_t vram[0x18000];      // 96 KB Video RAM
    uint8_t oam[0x400];         // 1 KB Object Attribute Memory
    uint8_t sram[0x10000];      // 64 KB Cartridge Backup RAM / Flash

    EEPROM eeprom;              // Serial EEPROM emulation

    uint8_t *rom;               // Dynamic Game Pak ROM buffer
    size_t rom_size;            // Size of loaded ROM in bytes
    char rom_title[16];         // Cartridge title extracted from header
    char save_path[512];        // Save file path (<rom_path>.sav)
    bool sram_dirty;            // SRAM modification flag
    uint32_t open_bus;          // Floating data bus value

    struct GBA *gba;            // Reference to master GBA structure
} GBABus;

void bus_init(GBABus *bus, struct GBA *gba);
bool bus_load_rom(GBABus *bus, const char *rom_path);
void bus_cleanup(GBABus *bus);

void bus_flush_sram(GBABus *bus);
void bus_load_sram(GBABus *bus);

uint8_t bus_read8(GBABus *bus, uint32_t addr);
uint16_t bus_read16(GBABus *bus, uint32_t addr);
uint32_t bus_read32(GBABus *bus, uint32_t addr);

void bus_write8(GBABus *bus, uint32_t addr, uint8_t val);
void bus_write16(GBABus *bus, uint32_t addr, uint16_t val);
void bus_write32(GBABus *bus, uint32_t addr, uint32_t val);

// Internal I/O helpers
uint8_t bus_read_io8(GBABus *bus, uint32_t addr);
uint16_t bus_read_io16(GBABus *bus, uint32_t addr);
uint32_t bus_read_io32(GBABus *bus, uint32_t addr);

void bus_write_io8(GBABus *bus, uint32_t addr, uint8_t val);
void bus_write_io16(GBABus *bus, uint32_t addr, uint16_t val);
void bus_write_io32(GBABus *bus, uint32_t addr, uint32_t val);

// EEPROM access over DMA3
uint16_t eeprom_read(GBABus *bus);
void eeprom_write(GBABus *bus, uint16_t val, int count);

#endif // BUS_H
