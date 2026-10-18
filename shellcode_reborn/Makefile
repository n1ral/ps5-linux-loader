ifeq ($(shell uname -m),aarch64)
CC      = x86_64-linux-gnu-gcc
AS      = x86_64-linux-gnu-as
LD      = x86_64-linux-gnu-ld
OBJCOPY = x86_64-linux-gnu-objcopy
else
CC      = gcc
AS      = as
LD      = ld
OBJCOPY = objcopy
endif

CFLAGS  = -O2 -fno-stack-protector -ffreestanding -nostdlib -fcf-protection=none \
          -mno-red-zone -mno-sse -mno-sse2 -m64 -fno-asynchronous-unwind-tables
LDFLAGS = -T link.ld

TARGET  = shellcode_reborn.elf
BIN     = shellcode_reborn.bin
DUMP    = shellcode_reborn.h

all: $(DUMP)

entry.o: entry.S
	$(CC) $(CFLAGS) -c $< -o $@

break_hv.o: break_hv.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): entry.o break_hv.o
	$(LD) $(LDFLAGS) entry.o break_hv.o -o $@

$(BIN): $(TARGET)
	$(OBJCOPY) -O binary -j .text $< $@

$(DUMP): $(BIN)
	xxd -i $(BIN) > $@

clean:
	rm -f entry.o break_hv.o $(TARGET) $(BIN) $(DUMP)
