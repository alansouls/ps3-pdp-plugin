# Build with the Sony PS3 SDK: export CELL_SDK=/path/to/cell && make
CELL_MK_DIR ?= $(CELL_SDK)/samples/mk
include $(CELL_MK_DIR)/sdk.makedef.mk

PPU_SRCS         = riffmaster_loader.c
PPU_PRX_TARGET   = riffmaster_loader.prx
PPU_SPRX_TARGET  = riffmaster_loader.sprx
PPU_CFLAGS      += -Os -std=gnu99 -fno-builtin -fno-tree-loop-distribute-patterns \
                   -ffunction-sections -fdata-sections
PPU_PRX_LDFLAGS += -Wl,--strip-unused-data
PPU_PRX_LDLIBS  += -lfs_stub

include $(CELL_MK_DIR)/sdk.target.mk

all: $(PPU_SPRX_TARGET)

$(PPU_SPRX_TARGET): $(PPU_PRX_TARGET)
	$(PPU_PRX_STRIP) --strip-debug --strip-section-header $<
	$(MAKE_FSELF) $< $@
