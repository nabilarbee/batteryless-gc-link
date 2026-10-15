# Builds the code parts of the two multiboot programs:
#   flasher_code.bin     the link program flasher (make_flasher.py adds the data)
#   savewriter_code.bin  the save writer (make_save_writer.py adds the data)
#   bldumper_code.bin    GBA side of the batteryless dumper (make_batteryless_dumper.py)
# Needs agbcc in ../../tools/agbcc (see the colosseum-mb INSTALL.md) and
# binutils for arm-none-eabi.

AGBCC   := ../../tools/agbcc
CC1     := $(AGBCC)/bin/agbcc
CC1_OLD := $(AGBCC)/bin/old_agbcc
PREFIX  := arm-none-eabi-
CPP     := $(PREFIX)cpp
AS      := $(PREFIX)as
LD      := $(PREFIX)ld
OBJCOPY := $(PREFIX)objcopy

CPPFLAGS := -I $(AGBCC)/include -iquote . -iquote ../../payload/include -nostdinc -undef
CFLAGS   := -O2 -mthumb-interwork -fhex-asm -Wimplicit -Werror
ASFLAGS  := -mcpu=arm7tdmi

BUILD := build
COMMON := $(BUILD)/crt0.o $(BUILD)/hash.o $(BUILD)/console.o $(BUILD)/cfi.o $(BUILD)/repro_cart.o
LIBS   := -L $(AGBCC)/lib -lgcc -lc

all: flasher_code.bin savewriter_code.bin bldumper_code.bin

# Copies go next to the make_*.py scripts, which is where they look for them.
flasher_code.bin: $(BUILD)/flasher.elf
	$(OBJCOPY) -O binary $< $@
	cp $@ ../flasher_code.bin

savewriter_code.bin: $(BUILD)/savewriter.elf
	$(OBJCOPY) -O binary $< $@
	cp $@ ../savewriter_code.bin

bldumper_code.bin: $(BUILD)/bldumper.elf
	$(OBJCOPY) -O binary $< $@
	@test $$(stat -c %s $@) -le 126976 || (echo "bldumper_code.bin overlaps the receive buffer" && rm $@ && false)
	cp $@ ../bldumper_code.bin

$(BUILD)/flasher.elf: $(BUILD)/main.o $(COMMON) flasher.ld
	$(LD) -T flasher.ld -Map $(BUILD)/flasher.map -o $@ $(BUILD)/crt0.o $(BUILD)/hash.o $(BUILD)/main.o $(BUILD)/console.o $(BUILD)/cfi.o $(BUILD)/repro_cart.o $(LIBS)

$(BUILD)/savewriter.elf: $(BUILD)/savewriter.o $(BUILD)/savecore.o $(COMMON) flasher.ld
	$(LD) -T flasher.ld -Map $(BUILD)/savewriter.map -o $@ $(BUILD)/crt0.o $(BUILD)/hash.o $(BUILD)/savewriter.o $(BUILD)/savecore.o $(BUILD)/console.o $(BUILD)/cfi.o $(BUILD)/repro_cart.o $(LIBS)

$(BUILD)/bldumper.elf: $(BUILD)/bldumper.o $(BUILD)/savecore.o $(BUILD)/bios.o $(COMMON) flasher.ld
	$(LD) -T flasher.ld -Map $(BUILD)/bldumper.map -o $@ $(BUILD)/crt0.o $(BUILD)/hash.o $(BUILD)/bldumper.o $(BUILD)/savecore.o $(BUILD)/bios.o $(BUILD)/console.o $(BUILD)/cfi.o $(BUILD)/repro_cart.o $(LIBS)

$(BUILD)/%.o: %.s | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD)/%.o: %.c *.h | $(BUILD)
	$(CPP) $(CPPFLAGS) $< -o $(BUILD)/$*.i
	$(CC1) $(CFLAGS) -o $(BUILD)/$*.s < $(BUILD)/$*.i
	$(AS) $(ASFLAGS) -o $@ $(BUILD)/$*.s

# The chip code is the save backend's, built the same way as in the payload.
$(BUILD)/repro_cart.o: ../../payload/src/repro/repro_cart.c ../../payload/include/repro_*.h | $(BUILD)
	$(CPP) $(CPPFLAGS) $< -o $(BUILD)/repro_cart.i
	$(CC1_OLD) $(CFLAGS) -o $(BUILD)/repro_cart.s < $(BUILD)/repro_cart.i
	$(AS) $(ASFLAGS) -o $@ $(BUILD)/repro_cart.s

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD) flasher_code.bin savewriter_code.bin bldumper_code.bin

.PHONY: all clean
