# Build with the Sony PS3 SDK: export CELL_SDK=/path/to/cell && make
CELL_MK_DIR ?= $(CELL_SDK)/samples/mk
include $(CELL_MK_DIR)/sdk.makedef.mk

# The SDK hardcodes Cygwin paths (/bin/cp); plain names also work with native make.
CP = cp
RM = rm -f
MV = mv

PPU_SRCS         = riffmaster_loader.c
PPU_PRX_TARGET   = riffmaster_loader.prx
PPU_SPRX_TARGET  = riffmaster_loader.sprx
PPU_CFLAGS      += -Os -std=gnu99 -fno-builtin \
                   -ffunction-sections -fdata-sections
# make NO_LOG=1: compile all logging out (no log file, no log buffer, no counters).
# Run make clean first when switching, the objects don't track this flag.
ifdef NO_LOG
PPU_CFLAGS      += -DRIFF_NO_LOG
endif
PPU_PRX_LDFLAGS += -Wl,--strip-unused-data
PPU_PRX_LDLIBS  += -lfs_stub

include $(CELL_MK_DIR)/sdk.target.mk

all: $(PPU_SPRX_TARGET)

$(PPU_SPRX_TARGET): $(PPU_PRX_TARGET)
	$(PPU_PRX_STRIP) --strip-debug --strip-section-header $<
	$(MAKE_FSELF) $< $@
