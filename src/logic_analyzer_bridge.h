#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool la_is_busy(void);

uint32_t la_configure_frequency(uint32_t *freq);
uint32_t la_configure_chunk_bytes(uint32_t *chunk_bytes);
uint8_t  la_configure_channel(size_t i, uint8_t *channel);
uint8_t  la_configure_channel_count(uint8_t *channel_count);
uint8_t  la_copy_channels(uint8_t *channels, size_t siz);

typedef struct {
  const uint8_t *buffer;
  uint32_t samples;
  uint32_t first_sample;
  uint8_t bytes_per_sample;
  const volatile uint32_t *timestamps;
  uint8_t timestamp_count;
} la_capture_result_t;

bool la_capture_start_simple(uint32_t pre_samples, uint32_t post_samples, uint8_t trigger_pin, bool invert_trigger);
bool la_capture_is_running(void);
bool la_capture_get_result(la_capture_result_t *result);
void la_capture_stop(void);

typedef struct {
  const uint8_t *data;
  uint32_t bytes;
  uint32_t samples;
  uint32_t sequence;
  uint32_t overruns;
  uint64_t first_sample;
  uint64_t device_time_us;
  uint8_t  slot;
} la_analysis_block_t;

bool la_capture_chain_start(void);
bool la_capture_chain_take(la_analysis_block_t *block);
void la_capture_chain_release(uint8_t slot);
void la_capture_chain_stop(void);

bool la_stream_start(void);
bool la_stream_take(la_analysis_block_t *block);
void la_stream_release(uint8_t slot);
void la_stream_stop(void);

uint32_t la_raw_high_count(void);

#ifdef __cplusplus
}
#endif
