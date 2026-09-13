# Additional makefile for the isolated headless build. Never used by the
# Falcon game build; substitutes only the terminal MIDI device implementation.
audio/null.o: $(srcdir)/devtools/atari-falcon030/tools/foa-faithful-music/trace-midi.cpp config.h
	@mkdir -p audio
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) -c $< -o $@
