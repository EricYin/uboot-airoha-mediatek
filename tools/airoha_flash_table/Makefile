###############################################################################
# Builds the flash_table_gen host tool, which serializes the SPI-NAND flash
# table at build time so BL2 can locate matching flash entries at runtime.
#
# The tool runs on the build host, so it links against host libc and is
# compiled with HOSTCC (gcc by default). CROSS_COMPILE is intentionally not
# used here.
###############################################################################

HOSTCC      ?= cc

# FLASH_TABLE_OPEN switches the source file to its host-tool entry point
# (main()), which writes the binary table to disk instead of registering it
# at runtime.
CFLAGS  := -O2 -Wall
CFLAGS  += -DFLASH_TABLE_OPEN
CFLAGS  += -I.

SRC     := spi_nand_flash_table.c
TARGET  := flash_table_gen

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) spi_nand_flash.h nand_flash_otp.h
	$(HOSTCC) $(CFLAGS) $(SRC) -o $@

clean:
	rm -f $(TARGET) *.o flash_table.bin
