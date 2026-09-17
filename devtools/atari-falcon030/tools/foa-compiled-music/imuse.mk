# Separate executable and clock object; never replace the normal backend object.
FCM_TOOLS := $(srcdir)/devtools/atari-falcon030/tools/foa-compiled-music
include $(srcdir)/devtools/atari-falcon030/tools/foa-faithful-music/trace.mk
fcm-null.o: $(srcdir)/backends/platform/null/null.cpp config.h $(FCM_TOOLS)/imuse.mk
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) -DFCM_IMUSE_TEST_CLOCK -c $< -o $@
scummvm-fcm-test: $(DETECT_OBJS) $(filter-out backends/platform/null/null.o,$(OBJS)) fcm-null.o
	$(QUIET_LINK)$(LD) $(LDFLAGS) $(PRE_OBJS_FLAGS) $+ $(POST_OBJS_FLAGS) $(LIBS) -o $@
