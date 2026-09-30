#!/usr/bin/env python3
"""
Ntutu Advance System - Test ROM Generator
Generates synthetic ARM and THUMB test ROMs with valid GBA headers for automated unit testing.
"""

import struct
import os

def create_gba_header(title="TESTROM"):
    # 0x00: 4-byte ARM branch instruction jumping to 0x080000C0 (offset +0xB8 = 0x2E words)
    header = bytearray(192)
    # B 0x080000C0 -> 0xEA00002E
    struct.pack_into("<I", header, 0x00, 0xEA00002E)
    
    # 0x04 - 0x9F: Nintendo Logo placeholder
    for i in range(0x04, 0xA0):
        header[i] = (i * 7) & 0xFF
        
    # 0xA0 - 0xAB: Game Title (12 chars)
    title_bytes = title.encode('ascii')[:12].ljust(12, b'\0')
    header[0xA0:0xAC] = title_bytes
    
    # 0xAC - 0xAF: Game Code
    header[0xAC:0xB0] = b'NTUT'
    # 0xB0 - 0xB1: Maker Code
    header[0xB0:0xB2] = b'01'
    header[0xB2] = 0x96 # Fixed value
    
    # Checksum at 0xBD
    chk = 0
    for i in range(0xA0, 0xBD):
        chk = (chk - header[i]) & 0xFF
    chk = (chk - 0x19) & 0xFF
    header[0xBD] = chk
    
    return header

def generate_arm_test_rom(filename):
    header = create_gba_header("ARMTEST")
    code = bytearray()
    
    # Assembly instructions placed at 0x080000C0:
    # 1. Arithmetic Test:
    #    mov r0, #100       (0xE3A00064)
    #    add r0, r0, #25    (0xE2800019) -> r0 = 125
    #    sub r1, r0, #50    (0xE2401032) -> r1 = 75
    #    cmp r1, #75        (0xE351004B) -> Z flag set
    # 2. Barrel Shifter Test:
    #    mov r2, #1         (0xE3A02001)
    #    mov r3, r2, lsl #4 (0xE1A03202) -> r3 = 16
    #    mov r4, r3, lsr #2 (0xE1A04123) -> r4 = 4
    #    mov r5, r4, asr #1 (0xE1A050C4) -> r5 = 2
    #    mov r6, r5, ror #1 (0xE1A060E5) -> r6 = 0x40000001
    # 3. Branch & BL Test:
    #    bl target          (0xEB000001) -> offset +1 instruction
    #    b done             (0xEA000001)
    # target:
    #    add r7, r0, #1     (0xE2807001) -> r7 = 126
    #    bx lr              (0xE12FFF1E)
    # done:
    #    b done             (0xEAFFFFFE)
    
    arm_insts = [
        0xE3A00064, # mov r0, #100
        0xE2800019, # add r0, r0, #25
        0xE2401032, # sub r1, r0, #50
        0xE351004B, # cmp r1, #75
        0xE3A02001, # mov r2, #1
        0xE1A03202, # mov r3, r2, lsl #4
        0xE1A04123, # mov r4, r3, lsr #2
        0xE1A050C4, # mov r5, r4, asr #1
        0xE1A060E5, # mov r6, r5, ror #1
        0xEB000000, # bl target: target is at 0xEC, PC is 0xEC -> offset 0
        0xEA000001, # b done: done is at 0xF4, PC is 0xF0 -> offset 1
        0xE2807001, # target: add r7, r0, #1
        0xE12FFF1E, # bx lr
        0xEAFFFFFE  # done: b done
    ]
    
    for inst in arm_insts:
        code.extend(struct.pack("<I", inst))
        
    rom = header + code
    # Pad to 32 KB minimum
    if len(rom) < 0x8000:
        rom += b'\0' * (0x8000 - len(rom))
        
    with open(filename, "wb") as f:
        f.write(rom)
    print(f"[TEST GEN] Generated {filename} ({len(rom)} bytes)")

def generate_thumb_test_rom(filename):
    header = create_gba_header("THUMBTEST")
    code = bytearray()
    
    # 0x080000C0: ARM prelude to switch to THUMB
    #    add r0, pc, #1   (0xE28F0001) -> r0 = 0x080000C9 (THUMB bit set)
    #    bx r0            (0xE12FFF10)
    # 0x080000C8: THUMB code starts
    #    mov r0, #40      (0x2028)
    #    add r0, #10      (0x300A) -> r0 = 50
    #    sub r1, r0, #20  (0x1F01) -> r1 = 30 (sub r1, r0, #0 / or sub imm)
    #    push {r0, r1}    (0xB403)
    #    pop {r2, r3}     (0xBC0C) -> r2 = 50, r3 = 30
    #    b finish         (0xE000)
    # finish:
    #    b finish         (0xE7FE)
    
    arm_prelude = [
        0xE28F0001, # add r0, pc, #1
        0xE12FFF10  # bx r0
    ]
    for inst in arm_prelude:
        code.extend(struct.pack("<I", inst))
        
    thumb_insts = [
        0x2028, # mov r0, #40
        0x300A, # add r0, #10
        0x2114, # mov r1, #20
        0x1842, # add r2, r0, r1 -> r2 = 70
        0xB405, # push {r0, r2}
        0xBC50, # pop {r4, r6} -> r4 = 50, r6 = 70
        0xE000, # b target (offset 0 -> target at PC = A + 4)
        0x2700, # skipped: mov r7, #0
        0x2701, # target: mov r7, #1
        0xE7FE  # loop: b loop
    ]
    for inst in thumb_insts:
        code.extend(struct.pack("<H", inst))
        
    rom = header + code
    if len(rom) < 0x8000:
        rom += b'\0' * (0x8000 - len(rom))
        
    with open(filename, "wb") as f:
        f.write(rom)
    print(f"[TEST GEN] Generated {filename} ({len(rom)} bytes)")

def generate_visual_demo_rom(filename):
    header = create_gba_header("DEMOGBA")
    code = bytearray()
    demo_insts = [
        0xE3A00401, # mov r0, #0x04000000
        0xE3A01B01, # mov r1, #0x0400
        0xE3811003, # orr r1, r1, #3       (0x0403: Mode 3 + BG2 enable)
        0xE1C010B0, # strh r1, [r0]
        0xE3A02406, # mov r2, #0x06000000  (VRAM base)
        0xE3A03000, # mov r3, #0           (counter)
        0xE3A0401F, # mov r4, #0x001F      (red color)
        # loop:
        0xE4824004, # str r4, [r2], #4
        0xE2844001, # add r4, r4, #1
        0xE2833001, # add r3, r3, #1
        0xE3530C4B, # cmp r3, #0x4B00      (19200 words = 38400 pixels)
        0x1AFFFFFB, # bne loop
        # spin:
        0xEAFFFFFE  # b spin
    ]
    for inst in demo_insts:
        code.extend(struct.pack("<I", inst))

    rom = header + code
    if len(rom) < 0x8000:
        rom += b'\0' * (0x8000 - len(rom))

    os.makedirs(os.path.dirname(filename), exist_ok=True)
    with open(filename, "wb") as f:
        f.write(rom)
    print(f"[TEST GEN] Generated {filename} ({len(rom)} bytes)")

if __name__ == "__main__":
    os.makedirs("tests", exist_ok=True)
    os.makedirs("roms", exist_ok=True)
    generate_arm_test_rom("tests/arm_test.gba")
    generate_thumb_test_rom("tests/thumb_test.gba")
    generate_visual_demo_rom("roms/game.gba")
    print("[TEST GEN] Synthetic test ROM generation complete.")
