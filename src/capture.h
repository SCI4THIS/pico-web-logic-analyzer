#pragma once

#include <cstdint>

namespace capture {

enum class Mode: uint8_t { channels8, channels16, channels24 };

struct Result {
  const uint8_t *buffer;
  uint32_t samples;
  uint32_t first_sample;
  uint8_t bytes_per_sample;
  const volatile uint32_t *timestamps;
  uint8_t timestamp_count;
};

bool start_simple(uint32_t frequency, uint32_t pre_samples,
                  uint32_t post_samples, const uint8_t *pins,
                  uint8_t pin_count, uint8_t trigger_pin,
                  bool invert_trigger = false,
                  Mode mode = Mode::channels8);

bool busy();
void stop();
Result result();

} // namespace capture
