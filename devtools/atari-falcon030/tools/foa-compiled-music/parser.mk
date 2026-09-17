# Include after the main Makefile in a configured host null-backend build.
FCM_TOOLS := $(srcdir)/devtools/atari-falcon030/tools/foa-compiled-music
ifeq ($(shell uname -s),Darwin)
FCM_TEST_LDFLAGS := -Wl,-dead_strip
endif
fcm-parser-test: $(FCM_TOOLS)/parser-test.cpp engines/scumm/imuse/imuse_fcm.o $(TEST_LIBS)
	$(QUIET_CXX)$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) -o $@ $< engines/scumm/imuse/imuse_fcm.o $(TEST_LIBS) $(TEST_LDFLAGS) $(FCM_TEST_LDFLAGS)
