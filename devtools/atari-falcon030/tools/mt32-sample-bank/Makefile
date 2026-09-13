# Offline capture only; Munt is never linked into the Falcon test program.
MUNT ?= $(CURDIR)/../../../../../F030MT32/third_party/munt
PARTIALS := ../mt32-partials
MUNT_NAMES := Analog BReverbModel Display File FileStream LA32FloatWaveGenerator LA32Ramp LA32WaveGenerator MidiStreamParser Part Partial PartialManager Poly ROMInfo Synth Tables TVA TVF TVP SampleRateConverter sha1
MUNT_OBJS := $(addprefix $(PARTIALS)/build/,$(addsuffix .o,$(MUNT_NAMES)))
CXXFLAGS ?= -O2 -std=c++11 -Wall -Wextra

all: build/capture

.PHONY: all munt
munt:
	$(MAKE) -C $(PARTIALS) MUNT=$(MUNT)

build/capture: capture.cpp munt
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I$(PARTIALS)/build -I$(PARTIALS)/build/mt32emu -I$(MUNT)/mt32emu/src capture.cpp $(MUNT_OBJS) -o $@
