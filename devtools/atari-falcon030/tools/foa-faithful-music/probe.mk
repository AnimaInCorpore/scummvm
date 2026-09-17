# Invoke after the normal Makefile in build-falcon030. A separate object
# provides the mixer definitions before libbackends.a; the normal mixer
# archive member is not extracted. The release executable stays unchanged.
PCM_PROBE_DIR := $(abspath $(srcdir)/devtools/atari-falcon030/tools/foa-faithful-music/build)
PCM_PROBE_OBJ := $(PCM_PROBE_DIR)/atari-mixer-probe.o
-include $(PCM_PROBE_OBJ:.o=.d)

$(PCM_PROBE_OBJ): $(srcdir)/backends/mixer/atari/atari-mixer.cpp $(srcdir)/backends/mixer/atari/atari-pcm-probe.h config.h
	@mkdir -p $(PCM_PROBE_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(DEFINES) $(INCLUDES) -DATARI_PCM_PROBE -MMD -MP -MF $(@:.o=.d) -c $< -o $@

.PHONY: pcm-probe
pcm-probe: $(PCM_PROBE_DIR)/PCMTEST.PRG
$(PCM_PROBE_DIR)/PCMTEST.PRG: $(PCM_PROBE_OBJ) $(DETECT_OBJS) $(OBJS) $(srcdir)/devtools/atari-falcon030/tools/foa-faithful-music/probe.mk
	$(LD) $(LDFLAGS) $(PRE_OBJS_FLAGS) $(PCM_PROBE_OBJ) $(DETECT_OBJS) $(OBJS) $(POST_OBJS_FLAGS) $(LIBS) -o $@
	$(NM) -n -C $@ | awk '$$2 ~ /^[TtW]$$/ && $$3 !~ /^\.L/' > $(PCM_PROBE_DIR)/probe-symbols.txt
	$(STRIP) -s $@
