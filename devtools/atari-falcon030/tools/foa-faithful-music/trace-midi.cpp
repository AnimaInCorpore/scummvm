// Offline capture backend, linked in place of audio/null.cpp only in the
// dedicated headless analysis build. ScummVM's iMUSE and MT-32 driver remain
// unchanged. No MIDI is sent to a physical device.
#include "audio/null.h"
#include "common/file.h"
#include "common/config-manager.h"
#include "common/events.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "common/timer.h"

class FalconMusicTrace : public MidiDriver_MPU401 {
public:
	FalconMusicTrace() : _open(false), _origin(0), _captureMs(20000), _quitSent(false) {}
	~FalconMusicTrace() override { close(); }
	int open() override {
		if (_open)
			return MERR_ALREADY_OPEN;
		if (!_output.open("imuse-midi.ev"))
			return MERR_CANNOT_CONNECT;
		_origin = g_system->getMillis(true);
		_open = true;
		if (ConfMan.hasKey("foa_capture_ms"))
			_captureMs = ConfMan.getInt("foa_capture_ms");
		if (_captureMs < 1000 || _captureMs > 600000)
			error("FoA capture duration must be 1–600 seconds");
		g_system->getTimerManager()->installTimerProc(captureTimer, 10000, this, "FoA MIDI capture limit");
		_output.writeString("# Post-iMUSE MT-32 device events; absolute microseconds, 1 ms timestamp resolution\n");
		return 0;
	}
	bool isOpen() const override { return _open; }
	void close() override {
		if (!_open)
			return;
		g_system->getTimerManager()->removeTimerProc(captureTimer);
		MidiDriver_MPU401::close();
		_output.writeString("# end\n");
		_output.finalize();
		if (_output.err())
			error("MIDI trace finalization failed");
		_output.close();
		_open = false;
	}
	void send(uint32 message) override {
		if (!_open)
			return;
		byte status = message & 255;
		Common::String line = Common::String::format("%llu %02x%02x",
			(unsigned long long)(g_system->getMillis(true) - _origin) * 1000,
			status, (message >> 8) & 127);
		if ((status & 0xf0) != 0xc0 && (status & 0xf0) != 0xd0)
			line += Common::String::format("%02x", (message >> 16) & 127);
		_output.writeString(line + "\n");
		if (_output.err())
			error("MIDI trace write failed");
	}
	void sysEx(const byte *data, uint16 length) override {
		if (!_open)
			return;
		Common::String line = Common::String::format("%llu f0",
			(unsigned long long)(g_system->getMillis(true) - _origin) * 1000);
		for (uint16 i = 0; i < length; ++i)
			line += Common::String::format("%02x", data[i]);
		_output.writeString(line + "f7\n");
		if (_output.err())
			error("MIDI trace SysEx write failed");
	}
private:
	static void captureTimer(void *context) {
		FalconMusicTrace *self = static_cast<FalconMusicTrace *>(context);
		if (!self->_quitSent && g_system->getMillis(true) - self->_origin >= self->_captureMs) {
			self->_output.writeString("# Capture limit; subsequent shutdown note-offs are harness-generated\n");
			self->_quitSent = true;
			Common::Event event;
			event.type = Common::EVENT_QUIT;
			g_system->getEventManager()->pushEvent(event);
		}
	}
	bool _open;
	uint32 _origin;
	uint32 _captureMs;
	bool _quitSent;
	Common::DumpFile _output;
};

const char *NullMusicPlugin::getName() const { return "Offline MT-32 event capture"; }
Common::Error NullMusicPlugin::createInstance(MidiDriver **driver, MidiDriver::DeviceHandle) const {
	*driver = new FalconMusicTrace();
	return Common::kNoError;
}
MusicDevices NullMusicPlugin::getDevices() const {
	MusicDevices devices;
	devices.push_back(MusicDevice(this, "", MT_MT32));
	return devices;
}
class AutoMusicPlugin : public NullMusicPlugin {
public:
	const char *getName() const override { return "Offline MT-32 event capture (default)"; }
	const char *getId() const override { return "auto"; }
};
REGISTER_PLUGIN_STATIC(AUTO, PLUGIN_TYPE_MUSIC, AutoMusicPlugin);
REGISTER_PLUGIN_STATIC(NULL, PLUGIN_TYPE_MUSIC, NullMusicPlugin);
