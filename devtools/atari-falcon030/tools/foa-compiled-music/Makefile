# Munt is linked only into the host oracle/exporter. runtime.h is independent.
MUNT ?= $(CURDIR)/../../../../../F030MT32/third_party/munt
PARTIALS := ../mt32-partials
MUNT_NAMES := Analog BReverbModel Display File FileStream LA32FloatWaveGenerator LA32Ramp LA32WaveGenerator MidiStreamParser Part Partial PartialManager Poly ROMInfo Synth Tables TVA TVF TVP SampleRateConverter sha1
MUNT_OBJS := $(addprefix $(PARTIALS)/build/,$(addsuffix .o,$(MUNT_NAMES)))
CXXFLAGS ?= -O2 -std=c++11 -Wall -Wextra -Werror
MINT_CXX ?= m68k-atari-mintelf-g++
INCLUDES := -I$(PARTIALS)/build -I$(PARTIALS)/build/mt32emu -I$(MUNT)/mt32emu/src
.PHONY: all munt test target-check
all: build/oracle build/scan

build/live-pcm-test: live-pcm-test.cpp live-demo.h live-resampler.h live-pcm.h runtime.h munt
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDES) live-pcm-test.cpp $(MUNT_OBJS) -o $@

.PHONY: live-test
live-test: build/live-pcm-test
	build/live-pcm-test kernel

build/live-demo: live-demo.cpp live-demo.h live-resampler.h live-pcm.h runtime.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) live-demo.cpp -o $@

.PHONY: live-target-check
live-target-check:
	@mkdir -p build
	$(MINT_CXX) -m68030 -msoft-float -O2 -std=c++11 -fno-exceptions -fno-rtti -Wall -Wextra -Werror live-demo.cpp -o build/LIVEDEMO.TOS
munt:
	$(MAKE) -C $(PARTIALS) MUNT=$(MUNT)
build/oracle: oracle.cpp runtime.h munt
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INCLUDES) oracle.cpp $(MUNT_OBJS) -o $@
build/scan: scan.cpp runtime.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) scan.cpp -o $@
test: all
	python3 -m unittest discover -s . -p 'test_*.py' -v
	build/oracle test
target-check:
	@mkdir -p build
	$(MINT_CXX) -m68030 -msoft-float -O2 -std=c++11 -ffreestanding -fno-exceptions -fno-rtti -Wall -Wextra -Werror -c target_probe.cpp -o build/target_probe.o
