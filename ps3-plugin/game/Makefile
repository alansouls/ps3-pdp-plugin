# Build with the Sony PS3 SDK: export CELL_SDK=/path/to/cell && make
CELL_MK_DIR ?= $(CELL_SDK)/samples/mk
include $(CELL_MK_DIR)/sdk.makedef.mk

# The SDK hardcodes Cygwin paths (/bin/cp); plain names also work with native make.
CP = cp
RM = rm -f
MV = mv

PPU_SRCS         = riffmaster_game.c
PPU_PRX_TARGET   = riffmaster_game.prx
PPU_SPRX_TARGET  = riffmaster_game.sprx
PPU_CFLAGS      += -Os -std=gnu99 -fno-builtin \
                   -ffunction-sections -fdata-sections
PPU_PRX_LDFLAGS += -Wl,--strip-unused-data
PPU_PRX_LDLIBS  += -lio_stub -lusbd_stub -lfs_stub -lsysmodule_stub

include $(CELL_MK_DIR)/sdk.target.mk

all: $(PPU_SPRX_TARGET)

$(PPU_SPRX_TARGET): $(PPU_PRX_TARGET)
	$(PPU_PRX_STRIP) --strip-debug --strip-section-header $<
	$(MAKE_FSELF) $< $@
