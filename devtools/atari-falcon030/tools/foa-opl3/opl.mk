# Additional makefile for the isolated headless capture build. It is never used
# by the Falcon game build and replaces no normal object: it links a separate
# executable from the ordinary objects, with the OPL factory swapped for the
# register capture and the null backend rebuilt with the virtual test clock.
OPL3_TOOLS := $(srcdir)/devtools/atari-falcon030/tools/foa-opl3

opl-null.o: $(srcdir)/backends/platform/null/null.cpp config.h $(OPL3_TOOLS)/opl.mk
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) -DFCM_IMUSE_TEST_CLOCK -c $< -o $@

opl-capture.o: $(OPL3_TOOLS)/trace-opl.cpp config.h $(OPL3_TOOLS)/opl.mk
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) -c $< -o $@

scummvm-opl-capture: $(DETECT_OBJS) \
		$(filter-out backends/platform/null/null.o audio/fmopl.o,$(OBJS)) \
		opl-null.o opl-capture.o
	$(QUIET_LINK)$(LD) $(LDFLAGS) $(PRE_OBJS_FLAGS) $+ $(POST_OBJS_FLAGS) $(LIBS) -o $@
