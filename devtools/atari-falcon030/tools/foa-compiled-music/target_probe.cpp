// Freestanding 68030 compilation gate for the live control primitives.
#include "runtime.h"
extern "C" uint32_t fcmRampAdvance(FCM::Ramp *ramp, uint32_t samples) {
	return ramp->advance(samples);
}
extern "C" void fcmRampStart(FCM::Ramp *ramp, uint8_t target, uint8_t increment, const uint8_t *table) {
	ramp->start(target, increment, table);
}
extern "C" void fcmTrackSeek(FCM::Track *track, uint32_t tick) {
	track->seek(tick);
}
