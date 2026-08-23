#include "LogicAnalyzer_Capture.c"
#include "logic_analyzer_bridge.h"

/*
 * Functions added here can access file-static capture internals because
 * they are now part of the same translation unit.
 */

void la_get_debug(uint32_t *tail, uint32_t *start, uint8_t *pin_count)
{
  *tail = lastTail;
  *start = lastStartPosition;
  *pin_count = lastCapturePinCount;
}

uint32_t la_raw_high_count(void)
{
  uint32_t count = 0;
  uint32_t total = lastPreSize + lastPostSize * (lastLoopCount + 1);
  uint32_t first;

  if (lastTail < total - 1) {
    first = CAPTURE_BUFFER_SIZE - total + lastTail + 1;
  } else {
    first = lastTail - total + 1;
  }

  uint8_t mask = 1u << (lastCapturePins[0] - INPUT_PIN_BASE);

  for (uint32_t i = 0; i< total; i++) {
    uint32_t position = (first + i) % CAPTURE_BUFFER_SIZE;
    if (captureBuffer[position] & mask) {
      count++;
    }
  }

  return count;
}


static uint32_t la_frequency = 1000;
static uint32_t la_chunk_bytes = 256;

static uint8_t la_channels[24];
static uint8_t la_channel_count;

static bool la_running;
static bool la_capture_active;
static bool la_block_outstanding;

static uint32_t la_sequence;
static uint64_t la_next_sample;

static bool la_start_chunk(void)
{
  gpio_pull_down(pinMap[0]);
  sleep_us(50);
  bool started = StartCaptureSimple(la_frequency, 0, la_chunk_bytes, 0, 0, la_channels, la_channel_count, 0, true, MODE_8_CHANNEL);
  if (started) {
    la_capture_active = true;
    sleep_us(50);
    gpio_pull_up(pinMap[0]);
  }
  return started;
}

bool la_capture_chain_start(void)
{
  if (la_running) {
    return false;
  }
  if (la_channel_count == 0 || la_chunk_bytes == 0) {
    return false;
  }
  la_sequence = 0;
  la_next_sample = 0;
  la_block_outstanding = false;
  la_capture_active = false;
  if (!la_start_chunk()) {
    return false;
  }
  la_running = true;
  return true;
}

bool la_capture_chain_take(la_capture_chain_block_t *block)
{
  if (!la_running || !block || la_block_outstanding || !la_capture_active) {
    return false;
  }
  if (IsCapturing()) {
    return false;
  }
  uint32_t samples;
  uint32_t first;
  CHANNEL_MODE mode;
  uint8_t *buffer = GetBuffer(&samples, &first, &mode);
  if (mode != MODE_8_CHANNEL) {
    return false;
  }
  block->data = buffer + first;
  block->bytes = samples;
  block->samples = samples;
  block->sequence = la_sequence++;
  block->overruns = 0;
  block->first_sample = la_next_sample;
  block->device_time_us = time_us_64();
  block->slot = 0;

  la_next_sample += samples;
  la_capture_active = false;
  la_block_outstanding = true;

  return true;
}

void la_capture_chain_release(uint8_t slot)
{
  (void)slot;

  if (!la_running || !la_block_outstanding) {
    return;
  }
  la_block_outstanding = false;
  la_start_chunk();
}

void la_capture_chain_stop(void)
{
  if (la_capture_active && IsCapturing()) {
    StopCapture();
  }
  gpio_disable_pulls(pinMap[0]);
  la_running = false;
  la_capture_active = false;
  la_block_outstanding = false;
}

bool la_capture_chain_configure(uint32_t frequency, uint32_t chunk_bytes, const uint8_t *channels, uint8_t channel_count)
{
  if (la_running || !channels || channel_count == 0 || channel_count > 24 || chunk_bytes == 0 || chunk_bytes > 8192) {
    return false;
  }
  la_frequency = frequency;
  la_chunk_bytes = chunk_bytes;
  la_channel_count = channel_count;
  memmove(la_channels, channels, channel_count);
  return true;
}
