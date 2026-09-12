# Builds the MT-32 partial-demand harness against the Munt vendored in
# F030MT32. Munt is used at build time only and nothing here goes near the
# Falcon binary.
#
#   make                            # build
#   make run ROMS=/path/to/roms     # measure (needs a ROM pair)
#   make clean
#
# MUNT defaults to the F030MT32 checkout beside this repository, matching how
# devtools/atari-ste finds Hatari and the cross toolchain.

MUNT   ?= $(CURDIR)/../../../../../F030MT32/third_party/munt
SRCDIR := $(MUNT)/mt32emu/src
BUILD  := build
ROMS   ?= $(MUNT)/../../roms
CUES   ?= $(BUILD)/cues

MUNT_SRCS := Analog BReverbModel Display File FileStream LA32FloatWaveGenerator \
             LA32Ramp LA32WaveGenerator MidiStreamParser Part Partial \
             PartialManager Poly ROMInfo Synth Tables TVA TVF TVP \
             SampleRateConverter
OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(MUNT_SRCS))) $(BUILD)/sha1.o \
        $(BUILD)/mt32-partials.o

# Munt's generated config.h: static build, C++ API only, no version tagging.
CONFIG := $(BUILD)/mt32emu/config.h
CXXFLAGS ?= -O2 -std=c++11 -Wall -Wno-unused-parameter
INCLUDES := -I$(BUILD) -I$(SRCDIR) -I$(BUILD)/mt32emu

all: $(BUILD)/mt32-partials

$(CONFIG):
	@mkdir -p $(dir $@)
	@printf '#ifndef MT32EMU_CONFIG_H\n#define MT32EMU_CONFIG_H\n' > $@
	@printf '#define MT32EMU_VERSION "2.8.2"\n' >> $@
	@printf '#define MT32EMU_VERSION_MAJOR 2\n#define MT32EMU_VERSION_MINOR 8\n' >> $@
	@printf '#define MT32EMU_VERSION_PATCH 2\n' >> $@
	@printf '#define MT32EMU_EXPORTS_TYPE 0\n' >> $@
	@printf '#define MT32EMU_WITH_VERSION_TAGGING 0\n' >> $@
	@printf '#endif\n' >> $@
	@# Munt includes its own headers as <mt32emu/...> in places.
	@mkdir -p $(BUILD)/mt32emu
	@for h in $(SRCDIR)/*.h; do ln -sf "$$h" $(BUILD)/mt32emu/; done

$(BUILD)/sha1.o: $(SRCDIR)/sha1/sha1.cpp $(CONFIG)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: $(SRCDIR)/%.cpp $(CONFIG)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/mt32-partials.o: mt32-partials.cpp $(CONFIG)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/mt32-partials: $(OBJS)
	$(CXX) $(OBJS) -o $@

# Exports the game's cues, then measures them.
$(CUES)/.stamp:
	@mkdir -p $(CUES)
	python3 ../foa-mt32-demand.py --export $(CUES) $(FOA_DATA_DIR)
	@touch $@

run: all $(CUES)/.stamp
	./$(BUILD)/mt32-partials $(ROMS) $(CUES) $(MAX_CUES)

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
