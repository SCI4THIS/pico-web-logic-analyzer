#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

bool la_configure(uint32_t frequency, uint32_t chunk_bytes, const uint8_t *channels, uint8_t channel_count);

bool la_capture_chain_start(void);
bool la_capture_chain_take(la_analysis_block_t *block);
void la_capture_chain_release(uint8_t slot);
void la_capture_chain_stop(void);

bool la_stream_start(void);
bool la_stream_take(la_analysis_block_t *block);
void la_stream_release(uint8_t slot);
void la_stream_stop(void);
bool la_stream_is_running(void);

uint32_t la_raw_high_count(void);

#ifdef __cplusplus
}
#endif
