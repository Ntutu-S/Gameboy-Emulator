# Ntutu Advance System (`ntutu_station`)

A cycle-accurate, 1:1 Game Boy Advance (GBA) emulator written in pure C11 from first principles for Linux (Ubuntu). Built with **zero external third-party libraries** (no SDL, no third-party game engines), utilizing native X11/XShm and standard ALSA sound architecture.

---

## Architecture Overview

```
                                 +-----------------------+
                                 |   X11 / ALSA Front    |
                                 +-----------+-----------+
                                             |
                                 +-----------v-----------+
                                 |       GBA Core        |
                                 +-----------+-----------+
                                             |
            +----------------+---------------+---------------+----------------+
            |                |               |               |                |
     +------v------+  +------v------+  +-----v-----+  +------v------+  +------v------+
     |   ARM7TDMI  |  |  Memory Bus |  |    PPU    |  |    APU      |  | DMA & Timer |
     |  (ARMv4T)   |  |   (GBABus)  |  | (240x160) |  |  (44.1kHz)  |  | Controllers |
     +-------------+  +-------------+  +-----------+  +-------------+  +-------------+
```

### 1. Processor Subsystem (ARM7TDMI - ARMv4T)
- **3-Stage Prefetch Pipeline**: Fetch, Decode, Execute tracking with `pipeline[0]`, `pipeline[1]`, and `branched` flag.
  - When instruction at address $A$ is executing, PC reads as $A + 8$ in ARM mode and $A + 4$ in THUMB mode.
  - Subsystem functions `arm7tdmi_fill_pipeline_arm()` and `arm7tdmi_fill_pipeline_thumb()`.
- **7 Operating Modes & Banking**: User, FIQ, IRQ, Supervisor (SVC), Abort (ABT), Undefined (UND), System.
  - Complete banked register swapping: R8_fiq–R14_fiq, SPSR_fiq, R13_irq, R14_irq, SPSR_irq, R13_svc, R14_svc, SPSR_svc, R13_abt, R14_abt, SPSR_abt, R13_und, R14_und, SPSR_und.
- **ALU & Barrel Shifter**:
  - Shifts: LSL, LSR, ASR, ROR, RRX. Immediate `#0` edge cases (LSR #0 = 32, ASR #0 = 32, ROR #0 = RRX).
  - Status flags: Negative ($N$), Zero ($Z$), Carry ($C$), Overflow ($V$), and State ($T$).
- **Complete Decoders**:
  - Full 32-bit ARM instruction set: Branch (B, BL, BX), ALU data processing (16 opcodes), Single/Halfword/Signed Data Transfers, Block Data Transfers (LDM, STM), Multiply (MUL, MLA, UMULL, SMULL, UMLAL, SMLAL), Swap (SWP/SWPB), Status Register Transfers (MRS, MSR), and SWI.
  - Full 16-bit THUMB instruction set: Formats 1–19 decoded directly into fast C handlers.
- **CRITICAL: IRQ Return Pipeline Refill**:
  - In `SUBS PC, LR, #4` and LDM PC restoring SPSR, inspects restored CPSR bit 5 (`FLAG_T`). Refills in THUMB mode if set or ARM mode if cleared.

### 2. Memory Bus & Cartridge Subsystem
- **Standard Memory Map**:
  - BIOS (16 KB), EWRAM (256 KB, mirrored), IWRAM (32 KB, mirrored), I/O Registers (1 KB), Palette RAM (1 KB), VRAM (96 KB), OAM (1 KB), Game Pak ROM (Waitstates 0/1/2, mirrored continuously modulo ROM size), SRAM / Backup (64 KB).
- **Hardware Bus Quirks**:
  - Unaligned LDR rotates right by $(addr \ \& \ 3) \times 8$.
  - Unaligned LDRH rotates right by 8.
  - Unaligned stores mask off alignment bits.
  - Palette 8-bit writes expand byte across halfword (`(val << 8) | val`).
  - Background VRAM (< 0x06010000) expands byte across halfword.
  - Sprite/OBJ VRAM (>= 0x06010000) ignores 8-bit writes.
  - OAM 8-bit writes are completely ignored.
  - BIOS open-bus protection when $PC \ge 0x00004000$.
- **Save Persistence**:
  - Battery-backed SRAM (32 KB / 64 KB) flushed to `<rom_path>.sav`.
  - Serial EEPROM (4Kbit / 64Kbit) bit 0 protocol over DMA3.

### 3. Picture Processing Unit (PPU)
- $240 \times 160$ scanline rasterizer matching exact GBA timings: 1232 cycles/line (960 HDraw, 272 HBlank), 228 lines/frame (160 visible, 68 VBlank) $\approx 59.73\text{ Hz}$.
- Video Modes 0 to 5 (4 Text BGs, Affine BGs, Mode 3/4/5 direct and paletted bitmaps with page flipping).
- 128 hardware sprites (Normal and Affine matrices, 1D/2D tile mapping, lower-index tie-breaking).
- **CRITICAL: Compositor Precedence Rule**:
  $$\text{OBJ} > \text{BG0} > \text{BG1} > \text{BG2} > \text{BG3}$$
  at equal priority level ($0..3$).
- Alpha blending, brightness modulation, and windowing (WIN0, WIN1, OBJWIN).

### 4. DMA & Timers
- 4 DMA channels with priority arbitration ($\text{DMA0} > \text{DMA1} > \text{DMA2} > \text{DMA3}$). Immediate, VBlank, HBlank, and Special triggers (DirectSound FIFO A/B and EEPROM transfers).
- 4 hardware timers with reload, prescalers (/1, /64, /256, /1024), cascade mode, IRQ overflow, and DirectSound FIFO pacing.

### 5. Interrupt Controller & HLE BIOS
- Registers: IME, IE, IF.
- **CRITICAL: HALT Wake-Up Condition**: CPU unhalts strictly when $(IE \ \& \ IF) \ne 0$.
- Built-in ARM trampoline at 0x00000018 with user handler dispatch via 0x03007FFC and BIOS flag recording at 0x03007FF8.
- Complete HLE BIOS functions (SoftReset, Div, Sqrt, ArcTan, CpuSet, LZ77, RL, etc.).
- **VRAM Decompression Buffering**: Staging buffer committing 16-bit halfwords to prevent VRAM corruption.
- Custom 90-frame starry boot intro displaying "NTUTU ADVANCE SYSTEM" with harmonic 2-tone chime.

### 6. Linux X11/XShm & ALSA Frontend
- Pure standard C11 using native Linux libraries (zero SDL dependency).
- Integer scaled $720 \times 480$ ($3\times$) window with MIT Shared Memory Extension (XShm) and automatic fallback.
- ALSA stereo audio playback at 44,100 Hz with underrun recovery (`snd_pcm_recover`).
- Monotonic clock frame pacing (59.73 Hz) and Fast-Forward mode.

---

## Controls

| GBA Button | Keyboard Key |
|---|---|
| **A** | `Z` / `X` |
| **B** | `A` / `S` |
| **L Trigger** | `Q` |
| **R Trigger** | `W` / `E` |
| **Start** | `Return` (Enter) |
| **Select** | `Space` / `Backspace` / `Shift_R` |
| **D-Pad** | `Up`, `Down`, `Left`, `Right` Arrows |
| **Fast-Forward (4x Turbo)** | `Tab` (Toggle) |
| **Fullscreen** | `F11` or `F` (Toggle) |

---

## Building and Running

### Prerequisites (Ubuntu Linux)
```bash
sudo apt-get update
sudo apt-get install -y build-essential libx11-dev libxext-dev libasound2-dev python3
```

### Build Emulator
```bash
make
```
Produces `build/ntutu_station` and symlink `build/gba_emulator`.

### Run a Game
```bash
./build/ntutu_station path/to/game.gba

# Launch in fullscreen mode:
./build/ntutu_station --fullscreen path/to/game.gba

# or skip the boot sequence:
./build/ntutu_station --skip-intro path/to/game.gba
```

### Run Hardware Verification Tests
```bash
make test
```
Executes Python ROM generator and runs all 18 automated unit tests covering bus, CPU, THUMB, DMA, and PPU subsystems.
