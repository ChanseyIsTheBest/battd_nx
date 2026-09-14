#---------------------------------------------------------------------------------
# BLOONS ADVENTURE TIME TD -- Nintendo Switch homebrew loader (wrapper port)
# Unity 2020.3.40f1 / IL2CPP / arm64-v8a. Retargeted from the cloverpit_nx /
# killerbean_nx so-loader lineage (MIT), itself descended from the
# TheOfficialFloW / Rinnegatamante Vita/Switch loader tradition.
#
# Ships NO game code and NO game assets. You supply libmain/libunity/libil2cpp
# and the assets tree from a copy of Bloons Adventure Time TD you legally own.
# See README.md and tools/stage_sd.py.
#
# Requires devkitA64 + devkitPro packages:
#   dkp-pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib switch-libpng switch-ffmpeg
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment. (export DEVKITPRO=/opt/devkitpro)")
endif

#---------------------------------------------------------------------------------
# THIS PROJECT BUILDS FROM ANY DIRECTORY.
#
# It does not need to live under $(DEVKITPRO); everything below is relative to
# $(CURDIR). Clone it wherever you like and run make there. The only environment
# requirement is DEVKITPRO, checked above.
#
# The ONE layout that cannot work is a path containing a space. devkitPro's
# base_rules and the generated .d dependency files do not quote paths, so a space
# makes the build fail somewhere deep with a message that names neither the space
# nor the file. Catch it here instead.
#---------------------------------------------------------------------------------
ifneq ($(words $(CURDIR)),1)
$(error Project path contains a space: "$(CURDIR)" -- devkitPro's build rules \
        cannot handle that. Move the project to a path with no spaces.)
endif
ifneq ($(words $(DEVKITPRO)),1)
$(error DEVKITPRO contains a space: "$(DEVKITPRO)" -- reinstall devkitPro to a \
        path with no spaces.)
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := battd_nx
APP_TITLE   := Bloons Adventure Time TD
APP_AUTHOR  := ChanseyIsTheBest
APP_VERSION := 1.0.0
# Icon is OPTIONAL.
#
# $(wildcard) yields an empty string when the file is not there, and NROFLAGS
# below then omits --icon entirely so elf2nro falls back to its built-in default.
# Hardcoding --icon made a missing icon.jpg a hard build failure with the
# singularly unhelpful message "Failed to open input icon!" -- which says nothing
# about which icon, or that it is optional. A repo should build from a clean
# checkout; an icon is a decoration, not a dependency.
APP_ICON    := $(wildcard $(TOPDIR)/icon.jpg)
export APP_TITLE APP_AUTHOR APP_VERSION APP_ICON
BUILD    := build
SOURCES  := source
INCLUDES := source
DATA     := data
# data/cacerts.pem is linked in with bin2o -> build/cacerts_pem.h (bp_net.c)
DATA     := data

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) \
           -DCRASH_LOG_PRINTF=stallPrintf \
           $(INCLUDE) -D__SWITCH__

# Promote the mistakes that are silent-but-fatal on AArch64 to hard errors.
#
# An implicitly-declared function is assumed to return int and take unspecified
# args. On AArch64 that means a function actually returning a pointer gets its
# result truncated to 32 bits, and the caller then dereferences a chopped
# address -- a crash a long way from the missing #include that caused it. GCC
# 14 makes this an error by default; devkitA64 is older, so ask explicitly.
#
# int-conversion and incompatible-pointer-types are the same shape of bug:
# both compile, both corrupt a value silently, and both are trivial to fix at
# the point the compiler notices them.
CFLAGS  += -Werror=implicit-function-declaration \
           -Werror=implicit-int \
           -Werror=int-conversion \
           -Werror=incompatible-pointer-types
# The reserved region where the game .so files are loaded. MUST stay in sync
# with so_util.c and with LOAD_ADDRESS in source/config.h -- three copies of one
# number, and a mismatch shows up as an unmapped-page abort during relocation
# rather than as anything that names the address.
CFLAGS  += -DLOAD_ADDRESS=0xC0000000
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := -g $(ARCH)

# Route nouveau's page-aligned GPU buffers into a dedicated contiguous arena
# rather than newlib's general heap. Without this they fragment it, and after a
# few minutes a ~20 MB contiguous run can no longer be placed -- which presents
# as a console freeze at a reproducible frame count, not as an allocation
# failure. Inherited from pvz_fusion_nx via cloverpit_nx.
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) \
            -Wl,--wrap,malloc -Wl,--wrap,calloc -Wl,--wrap,realloc \
            -Wl,--wrap,memalign -Wl,--wrap,free

#---------------------------------------------------------------------------------
# Dependency check.
#
# Fail early and say what to install. Without this the first sign of a missing
# package is a bare "SDL.h: No such file or directory" from whichever file
# happens to compile first, which names neither the package nor the fix.
# PORTLIBS is set by switch_rules, included above.
#---------------------------------------------------------------------------------
REQUIRED_HEADERS := \
    $(PORTLIBS)/include/SDL2/SDL.h \
    $(PORTLIBS)/include/GLES3/gl3.h \
    $(PORTLIBS)/include/EGL/egl.h \
    $(PORTLIBS)/include/png.h

MISSING := $(foreach h,$(REQUIRED_HEADERS),$(if $(wildcard $(h)),,$(h)))
ifneq ($(strip $(MISSING)),)
$(warning ==============================================================)
$(warning  Missing devkitPro portlibs headers:)
$(foreach h,$(MISSING),$(warning      $(h)))
$(warning )
$(warning  Install them -- from the devkitPro msys2 shell on Windows:)
$(warning      pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 \)
$(warning                switch-zlib switch-libpng)
$(warning  or on linux/macOS:)
$(warning      dkp-pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 \)
$(warning                    switch-zlib switch-libpng)
$(warning ==============================================================)
$(error missing portlibs -- see above)
endif

# mesa GLES3 + EGL + nouveau. The GLES path is forced: libunity.so has both
# backends compiled in but only libEGL.so is a hard DT_NEEDED, and libc_shim.c's
# dlopen refuses "libvulkan.so" so the engine falls back to GLES. The nouveau
# Vulkan driver is not a viable target here -- leave that refusal in place.
#
# ffmpeg IS needed here: BATTD ships four splash clips in assets/ and they ARE
# on the boot path (CNGamesLogo-*.mp4, NK_splash_sound*.mp4). See the block above.

#---------------------------------------------------------------------------------
# ffmpeg backs the splash-video player (source/battd_video.c), lifted from
# cloverpit_nx. ASK pkg-config RATHER THAN HARDCODING THE DEPENDENCY LIST:
# switch-ffmpeg is a static build whose transitive deps change between releases,
# and a stale hardcoded list surfaces as undefined references from inside
# libavcodec rather than as a missing package.
#
# With BATTD_VIDEO 0 in source/config.h the whole feature compiles to empty
# stubs that need no ffmpeg header and no ffmpeg library, and FFMPEG_LIBS
# becomes dead weight the linker discards.
#---------------------------------------------------------------------------------
PKGCONF := PKG_CONFIG_PATH=$(PORTLIBS)/lib/pkgconfig pkg-config
BATTD_VIDEO_ON := $(shell grep -cE '^[[:space:]]*\#define[[:space:]]+BATTD_VIDEO[[:space:]]+1' "$(TOPDIR)/source/config.h" 2>/dev/null)
ifneq ($(strip $(BATTD_VIDEO_ON)),0)
ifeq ($(wildcard $(PORTLIBS)/include/libavcodec/avcodec.h),)
$(warning ==============================================================)
$(warning  switch-ffmpeg is NOT INSTALLED, and BATTD_VIDEO is 1 in source/config.h)
$(warning )
$(warning  Install it:)
$(warning      dkp-pacman -S switch-ffmpeg      (msys2: pacman -S switch-ffmpeg))
$(warning )
$(warning  Or set BATTD_VIDEO 0 in source/config.h. The splash clips then do)
$(warning  not play and the intro is a black screen for its duration; nothing)
$(warning  else in the port depends on it.)
$(warning ==============================================================)
$(error switch-ffmpeg missing -- see above)
endif
FFMPEG_PKGS := libavformat libavcodec libswresample libswscale libavutil
FFMPEG_LIBS := $(shell $(PKGCONF) --static --libs $(FFMPEG_PKGS) 2>/dev/null)
ifeq ($(strip $(FFMPEG_LIBS)),)
FFMPEG_LIBS := -lavformat -lavcodec -lswresample -lswscale -lavutil -lbz2 -llzma
endif
endif

LIBS := -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau \
        -Wl,--start-group $(FFMPEG_LIBS) -Wl,--end-group \
        -lpng -lz -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))
export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(SFILES:.s=.o) $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)

#---------------------------------------------------------------------------------
# Object names are basenames, so foo.c and foo.s BOTH produce foo.o. When that
# happens OFILES_SRC lists it twice -- the link reports "multiple definition" of
# everything in the file, with the SAME .o named on both sides -- and make's
# pattern rules build only one of the two sources, so everything in the other
# becomes an undefined reference. Two unrelated-looking errors, one name clash.
# Cheap to detect, confusing to diagnose, so detect it.
#---------------------------------------------------------------------------------
ALLBASE := $(basename $(CFILES) $(CPPFILES) $(SFILES))
DUPBASE := $(strip $(foreach b,$(sort $(ALLBASE)),\
             $(if $(filter-out 1,$(words $(filter $(b),$(ALLBASE)))),$(b))))
ifneq ($(DUPBASE),)
$(error Duplicate object basename(s) in $(SOURCES): $(DUPBASE) -- two sources \
        map to the same .o. Rename one of them.)
endif
export OFILES  := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(PORTLIBS)/include/SDL2 -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean check
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

# Verify the staged game libraries against the shim tables before building.
# Run this after every game update: an unresolved import is not a link error in
# this design -- so_util taints the GOT slot and the game aborts at the first
# call site instead, typically minutes into a boot attempt with nothing in the
# log to say why.
#
#   make check GAME=out/battd
check:
	@test -n "$(GAME)" || (echo "usage: make check GAME=path/to/staged/libs"; exit 1)
	python3 tools/symcheck.py $(GAME) --source source

clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
NROFLAGS := --nacp=$(OUTPUT).nacp
ifneq ($(strip $(APP_ICON)),)
NROFLAGS += --icon=$(APP_ICON)
endif
all : $(OUTPUT).nro
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp $(APP_ICON)   # a new icon.jpg rebuilds the .nro
$(OUTPUT).elf : $(OFILES)

# generated headers must exist before any source compiles
$(OFILES_SRC) : $(HFILES_BIN)

%.pem.o %_pem.h : %.pem
	@echo $(notdir $<)
	@$(bin2o)
# data/cacerts.pem -> cacerts_pem.h (cacerts_pem, cacerts_pem_size), as acpc_nx does
$(OFILES_SRC) : $(HFILES_BIN)
%.pem.o %_pem.h : %.pem
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)
endif
