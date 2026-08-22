#include "capture.h"

extern "C" {
#include "LogicAnalyzer_Capture.h"
}

namespace capture {

static bool active = false;

static CHANNEL_MODE upstream_mode(Mode mode) {
  return static_cast<CHANNEL_MODE>(mode);
}

bool start_simple(uint32_t frequency, uint32_t pre_samples,
                  uint32_t post_samples, const uint8_t *pins,
                  uint8_t pin_count, uint8_t trigger_pin,
                  bool invert_trigger, Mode mode) {
  active = StartCaptureSimple(frequency, pre_samples, post_samples, 0, 0, pins, pin_count, trigger_pin, invert_trigger, upstream_mode(mode));
  return active;
}

bool busy() {
  return active && IsCapturing();
}

void stop() {
  StopCapture();
  active = false;
}

Result result() {
  uint32_t samples = 0;
  uint32_t first = 0;
  CHANNEL_MODE mode = MODE_8_CHANNEL;
  uint8_t timestamp_count = 0;

  const uint8_t *buffer = GetBuffer(&samples, &first, &mode);
  const volatile uint32_t *timestamps = GetTimestamps(&timestamp_count);
  const uint8_t width = mode == MODE_8_CHANNEL ? 1 : mode == MODE_16_CHANNEL ? 2 : 4;

  active = false;
  return { buffer, samples, first, width, timestamps, timestamp_count };
}

} // namespace capture

