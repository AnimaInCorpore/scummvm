# Supplemental live-synthesis executable using the Atari mixer and probe.
# Invoke after build-falcon030/Makefile. The release archive stays unchanged.
FCM_LIVE_DIR := $(abspath $(srcdir)/devtools/atari-falcon030/tools/foa-compiled-music)
FCM_LIVE_OBJ := $(FCM_LIVE_DIR)/build/atari-mixer-live.o
-include $(FCM_LIVE_OBJ:.o=.d)

$(FCM_LIVE_OBJ): $(srcdir)/backends/mixer/atari/atari-mixer.cpp $(srcdir)/backends/mixer/atari/atari-pcm-probe.h $(FCM_LIVE_DIR)/live-pcm.h $(FCM_LIVE_DIR)/live-demo.h $(FCM_LIVE_DIR)/live-resampler.h config.h
	@mkdir -p $(FCM_LIVE_DIR)/build
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) -DATARI_PCM_PROBE -DATARI_FCM_LIVE_PROBE -MMD -MP -MF $(@:.o=.d) -c $< -o $@

.PHONY: fcm-live-probe
fcm-live-probe: $(FCM_LIVE_DIR)/build/FCMLIVE.PRG
$(FCM_LIVE_DIR)/build/FCMLIVE.PRG: $(FCM_LIVE_OBJ) $(DETECT_OBJS) $(OBJS) $(FCM_LIVE_DIR)/live.mk
	$(LD) $(LDFLAGS) $(PRE_OBJS_FLAGS) $(FCM_LIVE_OBJ) $(DETECT_OBJS) $(OBJS) $(POST_OBJS_FLAGS) $(LIBS) -o $@
	$(NM) -n -C $@ | awk '$$2 ~ /^[TtW]$$/ && $$3 !~ /^\.L/' > $(FCM_LIVE_DIR)/build/live-symbols.txt
	$(STRIP) -s $@
