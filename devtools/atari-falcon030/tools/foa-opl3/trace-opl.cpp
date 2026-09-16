// Offline OPL register capture, linked in place of audio/fmopl.o only in the
// dedicated headless analysis executable. iMUSE and the real AdLib driver in
// audio/adlib.cpp are unchanged; this records what that driver already decided
// to send, after its own register cache has removed redundant writes.
//
// The capture is not an emulator: it synthesizes nothing and produces no audio.
// It deliberately does not derive its callback clock from the mixer. The AdLib
// driver asks for 250 Hz and the Falcon backend has to preserve those evenly
// spaced 4 ms events, so the capture drives them from the timer manager at the
// requested frequency and timestamps every write on that exact grid.
#include "audio/fmopl.h"

#include "common/config-manager.h"
#include "common/events.h"
#include "common/file.h"
#include "common/str.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "common/timer.h"

namespace OPL {

// Shared state that the real audio/fmopl.cpp also defines. Only the factory is
// replaced here; the OPL3 dual-OPL2 helpers are not, because no OPL backend
// that uses them is reachable in this executable.
bool OPL::_hasInstance = false;

OPL::OPL() {
	if (_hasInstance)
		error("There are multiple OPL output instances running");
	_hasInstance = true;
	_rhythmMode = false;
	_connectionFeedbackValues[0] = 0;
	_connectionFeedbackValues[1] = 0;
	_connectionFeedbackValues[2] = 0;
}

namespace Capture {

// Record kinds in the trace, one per line:
//   T <us>                 driver callback tick boundary
//   W <us> <ctx> <rrr> <vv> register write; ctx is 't' inside a callback,
//                          'g' from the engine thread between callbacks
//   I <us> <event>         open/init/reset/stop marker
class TraceOPL : public ::OPL::OPL {
public:
	TraceOPL(Config::OplType type) : _type(type), _freq(0), _tick(0), _inCallback(false),
		_writes(0), _ticks(0), _origin(0), _captureMs(60000), _quitSent(false), _started(false) {}

	~TraceOPL() override {
		stop();
		if (_output.isOpen()) {
			mark("close");
			_output.writeString(Common::String::format("# writes %u ticks %u\n# end\n", _writes, _ticks));
			_output.finalize();
			if (_output.err())
				error("OPL trace finalization failed");
			_output.close();
		}
	}

	bool init() override {
		if (_output.isOpen())
			return true;
		if (!_output.open("opl-writes.ev"))
			error("Cannot create opl-writes.ev");
		if (ConfMan.hasKey("foa_capture_ms"))
			_captureMs = ConfMan.getInt("foa_capture_ms");
		if (_captureMs < 1000 || _captureMs > 600000)
			error("FoA capture duration must be 1-600 seconds");
		_origin = g_system->getMillis(true);
		_output.writeString("# Post-AdLib-driver OPL register writes\n");
		_output.writeString(Common::String::format("# opl_type %d capture_ms %u\n", (int)_type, _captureMs));
		_output.writeString("# T <us> tick | W <us> <ctx> <reg> <val> | I <us> <event>\n");
		mark("init");
		return true;
	}

	void reset() override { mark("reset"); }

	// The AdLib driver only uses writeReg(). A port write is still recorded so
	// an unexpected caller cannot silently disappear from the trace; the latch
	// models the primary bank only, which is all an OPL2 engine can reach.
	void write(int a, int v) override {
		if ((a & 1) == 0)
			_port = v & 0xff;
		else
			writeReg(_port, v);
	}

	void writeReg(int r, int v) override {
		if (!_output.isOpen())
			return;
		++_writes;
		_output.writeString(Common::String::format("W %llu %c %03x %02x\n",
			(unsigned long long)now(), _inCallback ? 't' : 'g', r & 0x1ff, v & 0xff));
		if (_output.err())
			error("OPL trace write failed");
	}

	void setCallbackFrequency(int timerFrequency) override {
		stopCallbacks();
		startCallbacks(timerFrequency);
	}

protected:
	void startCallbacks(int timerFrequency) override {
		if (timerFrequency <= 0 || timerFrequency > 1000000)
			error("Unsupported OPL callback frequency %d", timerFrequency);
		_freq = (uint32)timerFrequency;
		_tick = 0;
		_started = true;
		mark(Common::String::format("start %d Hz", timerFrequency).c_str());
		// No 100 Hz cap and no batching: the interval is exactly the period the
		// driver asked for, so every callback keeps its own timestamp.
		g_system->getTimerManager()->installTimerProc(timerProc, 1000000 / timerFrequency, this, "OPL capture");
	}

	void stopCallbacks() override {
		if (!_started)
			return;
		g_system->getTimerManager()->removeTimerProc(timerProc);
		_started = false;
		mark("stop");
		_freq = 0;
	}

private:
	// Exact position on the callback grid while a callback runs, and the
	// virtual millisecond otherwise. Both clocks are the same virtual clock.
	uint64 now() const {
		if (_inCallback && _freq)
			return (uint64)_tick * 1000000ull / _freq;
		return (uint64)(g_system->getMillis(true) - _origin) * 1000ull;
	}

	void mark(const char *event) {
		if (!_output.isOpen())
			return;
		_output.writeString(Common::String::format("I %llu %s\n", (unsigned long long)now(), event));
	}

	static void timerProc(void *refCon) { static_cast<TraceOPL *>(refCon)->onTick(); }

	void onTick() {
		++_ticks;
		_inCallback = true;
		_output.writeString(Common::String::format("T %llu\n", (unsigned long long)now()));
		if (_callback && _callback->isValid())
			(*_callback)();
		_inCallback = false;
		++_tick;
		if (!_quitSent && g_system->getMillis(true) - _origin >= _captureMs) {
			_output.writeString("# Capture limit; later writes are harness-generated shutdown\n");
			_quitSent = true;
			Common::Event event;
			event.type = Common::EVENT_QUIT;
			g_system->getEventManager()->pushEvent(event);
		}
	}

	Config::OplType _type;
	uint32 _freq;
	uint32 _tick;
	bool _inCallback;
	uint32 _writes;
	uint32 _ticks;
	uint32 _origin;
	uint32 _captureMs;
	bool _quitSent;
	bool _started;
	int _port = 0;
	Common::DumpFile _output;
};

} // End of namespace Capture

// Single-driver factory. Emulators are still compiled into this executable but
// are unreachable, so a misconfigured run cannot silently record nothing.
// The id stays above the null and auto ids that audio/adlib.cpp tests for when
// it decides whether an AdLib device exists at all.
enum { kNull = 0, kAuto = 1, kCapture = 2 };

const Config::EmulatorDescription Config::_drivers[] = {
	{ "capture", "Offline OPL register capture", kCapture, kFlagOpl2 | kFlagDualOpl2 | kFlagOpl3 },
	{ nullptr, nullptr, 0, 0 }
};

Config::DriverId Config::parse(const Common::String &name) {
	if (name.equalsIgnoreCase("auto"))
		return kAuto;
	return name.equalsIgnoreCase("capture") ? (DriverId)kCapture : (DriverId)-1;
}

const Config::EmulatorDescription *Config::findDriver(DriverId id) {
	return id == kCapture ? &_drivers[0] : nullptr;
}

Config::DriverId Config::detect(OplType) { return kCapture; }

OPL *Config::create(OplType type) { return create(kCapture, type); }

OPL *Config::create(DriverId, OplType type) { return new Capture::TraceOPL(type); }

} // End of namespace OPL
