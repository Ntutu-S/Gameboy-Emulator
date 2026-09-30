CC = gcc
CFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra -O2 -Iinclude -g
LDFLAGS = -lX11 -lXext -lasound -lm -lpthread

TARGET = build/ntutu_station
SYMLINK = build/gba_emulator
TEST_TARGET = build/test_runner

SRCS = src/arm7tdmi.c \
       src/arm_instructions.c \
       src/thumb_instructions.c \
       src/bus.c \
       src/ppu.c \
       src/apu.c \
       src/dma.c \
       src/timer.c \
       src/interrupt.c \
       src/keypad.c \
       src/bios.c \
       src/frontend_x11.c \
       src/gba.c \
       src/main.c

EMU_SRCS = $(filter-out src/main.c, $(SRCS))
TEST_SRCS = tests/test_runner.c $(EMU_SRCS)

all: $(TARGET)

$(TARGET): $(SRCS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)
	@ln -sf ntutu_station $(SYMLINK)
	@echo "Build complete: $(TARGET) (symlinked to $(SYMLINK))"

test: $(TEST_TARGET)
	@echo "Generating synthetic test ROMs..."
	@python3 tests/generate_test_roms.py
	@echo "Running hardware verification tests..."
	@./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRCS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(TEST_SRCS) -o $(TEST_TARGET) $(LDFLAGS)

clean:
	rm -rf build tests/*.gba tests/*.bin

.PHONY: all test clean
