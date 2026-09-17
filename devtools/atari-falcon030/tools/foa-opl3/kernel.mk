# Additional makefile for the host kernel gate. It builds only an extra test
# executable in a configured null-backend build and replaces no normal object.
OPL3_TOOLS := $(srcdir)/devtools/atari-falcon030/tools/foa-opl3
OPL3_GEN := $(OPL3_TOOLS)/opl-tables.h
OPL3_GEN_PRACTICAL := $(OPL3_TOOLS)/opl-practical-tables.h
ifeq ($(shell uname -s),Darwin)
OPL3_TEST_LDFLAGS := -Wl,-dead_strip
endif

$(OPL3_GEN): $(OPL3_TOOLS)/generate-tables.py
	@mkdir -p $(OPL3_TOOLS)/build
	python3 $< --header $@ --dsp $(OPL3_TOOLS)/build/opltabs.inc

$(OPL3_GEN_PRACTICAL): $(OPL3_TOOLS)/generate-tables.py
	@mkdir -p $(OPL3_TOOLS)/build
	python3 $< --practical-header $@ --practical-dsp $(OPL3_TOOLS)/build/oplrt-tabs.inc

opl-practical-test: $(OPL3_TOOLS)/practical-test.cpp $(OPL3_TOOLS)/opl-practical.h $(OPL3_TOOLS)/opl-kernel.h $(OPL3_GEN) $(OPL3_GEN_PRACTICAL) $(TEST_LIBS)
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) \
		-I$(OPL3_TOOLS) -I$(OPL3_TOOLS)/build -o $@ $< $(TEST_LIBS) $(TEST_LDFLAGS) $(OPL3_TEST_LDFLAGS)

# Standalone: the two kernels and nothing of ScummVM.
opl-practical-unit-test: $(OPL3_TOOLS)/practical-unit-test.cpp $(OPL3_TOOLS)/opl-practical.h $(OPL3_TOOLS)/opl-kernel.h $(OPL3_GEN) $(OPL3_GEN_PRACTICAL)
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) -I$(OPL3_TOOLS) -o $@ $<

opl-rt-fixture: $(OPL3_TOOLS)/rt-fixture.cpp $(OPL3_TOOLS)/opl-practical.h $(OPL3_TOOLS)/opl-kernel.h $(OPL3_GEN) $(OPL3_GEN_PRACTICAL) $(TEST_LIBS)
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) \
		-I$(OPL3_TOOLS) -I$(OPL3_TOOLS)/build -o $@ $< $(TEST_LIBS) $(TEST_LDFLAGS) $(OPL3_TEST_LDFLAGS)

opl-kernel-test: $(OPL3_TOOLS)/kernel-test.cpp $(OPL3_TOOLS)/opl-kernel.h $(OPL3_GEN) $(TEST_LIBS)
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) \
		-I$(OPL3_TOOLS) -I$(OPL3_TOOLS)/build -o $@ $< $(TEST_LIBS) $(TEST_LDFLAGS) $(OPL3_TEST_LDFLAGS)

opl-dsp-fixture: $(OPL3_TOOLS)/dsp-fixture.cpp $(OPL3_TOOLS)/opl-kernel.h $(OPL3_GEN) $(TEST_LIBS)
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) \
		-I$(OPL3_TOOLS) -I$(OPL3_TOOLS)/build -o $@ $< $(TEST_LIBS) $(TEST_LDFLAGS) $(OPL3_TEST_LDFLAGS)
