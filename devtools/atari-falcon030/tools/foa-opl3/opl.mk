# Additional makefile for the isolated headless capture build. It is never used
# by the Falcon game build and replaces no normal object: it links a separate
# executable from the ordinary objects, with the OPL factory swapped for the
# register capture and the null backend rebuilt with the virtual test clock.
OPL3_TOOLS := $(srcdir)/devtools/atari-falcon030/tools/foa-opl3

opl-null.o: $(srcdir)/backends/platform/null/null.cpp config.h $(OPL3_TOOLS)/opl.mk
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) -DFCM_IMUSE_TEST_CLOCK -c $< -o $@

opl-capture.o: $(OPL3_TOOLS)/trace-opl.cpp config.h $(OPL3_TOOLS)/opl.mk
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) -c $< -o $@

# On Windows the null backend's file code calls the Win32 string and file
# helpers, which only the SDL backend builds.
ifdef WIN32
OPL3_HOST_OBJS := opl-win32-wrapper.o

opl-win32-wrapper.o: $(srcdir)/backends/platform/sdl/win32/win32_wrapper.cpp config.h $(OPL3_TOOLS)/opl.mk
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) -c $< -o $@
endif

# The replacements come first: the normal objects are module archives, and
# GNU ld resolves an archive only against what precedes it, and takes no
# member for a symbol these already define.
scummvm-opl-capture: opl-null.o opl-capture.o $(OPL3_HOST_OBJS) $(DETECT_OBJS) \
		$(filter-out backends/platform/null/null.o audio/fmopl.o,$(OBJS))
	$(QUIET_LINK)$(LD) $(LDFLAGS) $(PRE_OBJS_FLAGS) $+ $(POST_OBJS_FLAGS) $(LIBS) -o $@
