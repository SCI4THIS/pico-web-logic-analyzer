#define dma_handler la_original_dma_handler
#include "LogicAnalyzer_Capture.c"
#undef dma_handler
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

#define LA_MAX_CHANNELS 24

struct {
  uint32_t frequency;
  uint32_t chunk_bytes;
  uint8_t channel_count;
  uint8_t channels[LA_MAX_CHANNELS];
} la_config = {
  .frequency = 1000,
  .chunk_bytes = 256,
  .channel_count = 1,
  .channels = {1},
};

static bool la_running;
static bool la_capture_active;
static bool la_block_outstanding;
static bool la_stream_running;

static uint32_t la_sequence;
static uint64_t la_next_sample;

bool la_is_busy(void)
{
  return la_running || la_stream_running;
}

uint32_t la_configure_frequency(uint32_t *freq)
{
  if (freq != NULL && !la_is_busy()) {
    if (0 < *freq && *freq <= MAX_FREQ) {
      la_config.frequency = *freq;
    }
  }
  return la_config.frequency;
}

uint32_t la_configure_chunk_bytes(uint32_t *chunk_bytes)
{
  if (chunk_bytes != NULL && !la_is_busy()) {
    if (0 < *chunk_bytes && *chunk_bytes <= 8192) {
      la_config.chunk_bytes = *chunk_bytes;
    }
  }
  return la_config.chunk_bytes;
}

uint8_t la_configure_channel(size_t i, uint8_t *channel)
{
  if (i >= la_config.channel_count) {
    return 0xFF;
  }
  if (channel != NULL && !la_is_busy()) {
    if (*channel < LA_MAX_CHANNELS) {
      la_config.channels[i] = *channel;
    }
  }
  return la_config.channels[i];
}

uint8_t la_configure_channel_count(uint8_t *channel_count)
{
  if (channel_count != NULL && !la_is_busy()) {
    if (*channel_count <= LA_MAX_CHANNELS) {
      la_config.channel_count = *channel_count;
    }
  }
  return la_config.channel_count;
}

uint8_t la_copy_channels(uint8_t *channel_out, size_t channel_out_size)
{
  size_t i;
  size_t n = channel_out_size;
  if (la_config.channel_count < n) {
    n = la_config.channel_count;
  }
  for (i=0; i<n; i++) {
    channel_out[i] = la_configure_channel(i, NULL);
  }
  return n;
}

static bool la_start_chunk(void)
{
  gpio_pull_down(pinMap[0]);
  sleep_us(50);
  bool started = StartCaptureSimple(
      la_config.frequency, 0, la_config.chunk_bytes, 0, 0,
      la_config.channels, la_config.channel_count, 0, true,
      MODE_8_CHANNEL);
  if (started) {
    la_capture_active = true;
    sleep_us(50);
    gpio_pull_up(pinMap[0]);
  }
  return started;
}

bool la_capture_chain_start(void)
{
  if (la_is_busy()) {
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

bool la_capture_chain_take(la_analysis_block_t *block)
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

static uint16_t la_stream_pio_instructions[2];

static const struct pio_program la_stream_pio_program = {
  .instructions = la_stream_pio_instructions,
  .length = 2,
  .origin = -1,
};

static bool la_stream_block_outstanding;

static volatile uint32_t la_stream_completed_passes;
static uint64_t la_stream_next_chunk;
static uint64_t la_stream_last_produced_bytes;
static uint32_t la_stream_overruns;

static void __not_in_flash_func(la_stream_dma_handler)(void)
{
  la_original_dma_handler();
  la_stream_completed_passes++;
}

static uint64_t la_stream_produced_bytes(void)
{
  uint32_t passes = la_stream_completed_passes;
  uint32_t remaining;

  if (dma_channel_is_busy(dmaPingPong0)) {
    remaining = dma_channel_hw_addr(dmaPingPong0)->transfer_count;
  } else if (dma_channel_is_busy(dmaPingPong1)) {
    remaining = dma_channel_hw_addr(dmaPingPong1)->transfer_count;
  } else {
    return la_stream_last_produced_bytes;
  }

  uint64_t produced =
      (uint64_t)passes * CAPTURE_BUFFER_SIZE +
      (CAPTURE_BUFFER_SIZE - remaining);

  /* The next DMA can start just before its completion IRQ is serviced. */
  if (produced < la_stream_last_produced_bytes) {
    return la_stream_last_produced_bytes;
  }

  la_stream_last_produced_bytes = produced;
  return produced;
}

static void la_stream_stop_hardware(void)
{
  pio_sm_set_enabled(capturePIO, sm_Capture, false);

  hw_clear_bits(&dma_hw->ch[dmaPingPong0].al1_ctrl,
                DMA_CH0_CTRL_TRIG_EN_BITS);
  hw_clear_bits(&dma_hw->ch[dmaPingPong1].al1_ctrl,
                DMA_CH0_CTRL_TRIG_EN_BITS);

  dma_channel_set_irq0_enabled(dmaPingPong0, false);
  dma_channel_set_irq0_enabled(dmaPingPong1, false);
  irq_set_enabled(DMA_IRQ_0, false);

  dma_channel_abort(dmaPingPong0);
  dma_channel_abort(dmaPingPong1);
  dma_channel_acknowledge_irq0(dmaPingPong0);
  dma_channel_acknowledge_irq0(dmaPingPong1);

  irq_remove_handler(DMA_IRQ_0, la_stream_dma_handler);

  dma_channel_unclaim(dmaPingPong0);
  dma_channel_unclaim(dmaPingPong1);

  pio_sm_unclaim(capturePIO, sm_Capture);
  pio_remove_program(capturePIO, &la_stream_pio_program, captureOffset);
}

bool la_stream_start(void)
{
  if (la_is_busy()) {
    return false;
  }

  memset(captureBuffer, 0, sizeof(captureBuffer));

  la_stream_completed_passes = 0;
  la_stream_next_chunk = 0;
  la_stream_last_produced_bytes = 0;
  la_stream_overruns = 0;
  la_stream_block_outstanding = false;

  la_stream_pio_instructions[0] = pio_encode_in(pio_pins, 32);
  la_stream_pio_instructions[1] = pio_encode_jmp(0);

  capturePIO = pio0;
  pio_clear_instruction_memory(capturePIO);

  sm_Capture = pio_claim_unused_sm(capturePIO, true);
  pio_sm_clear_fifos(capturePIO, sm_Capture);
  pio_sm_restart(capturePIO, sm_Capture);
  captureOffset = pio_add_program(capturePIO, &la_stream_pio_program);

  /* MODE_8_CHANNEL samples board channels 1-8 / GPIO2-GPIO9. */
  for (uint8_t i = 0; i < 8; i++) {
    pio_gpio_init(capturePIO, INPUT_PIN_BASE + i);
    pio_sm_set_consecutive_pindirs(
        capturePIO, sm_Capture, INPUT_PIN_BASE + i, 1, false);
  }

  pio_sm_config config = pio_get_default_sm_config();
  sm_config_set_wrap(&config, captureOffset, captureOffset + 1);
  sm_config_set_in_pins(&config, INPUT_PIN_BASE);
  float clkdiv =
      (float)clock_get_hz(clk_sys) / (float)(la_config.frequency * 2u);
  sm_config_set_clkdiv(&config, clkdiv);
  sm_config_set_in_shift(&config, true, true, 32);
  sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_RX);

  pio_sm_init(capturePIO, sm_Capture, captureOffset, &config);

  configureCaptureDMAs(MODE_8_CHANNEL);

  irq_set_enabled(DMA_IRQ_0, false);
  irq_remove_handler(DMA_IRQ_0, la_original_dma_handler);
  irq_set_exclusive_handler(DMA_IRQ_0, la_stream_dma_handler);
  irq_set_enabled(DMA_IRQ_0, true);

  la_stream_running = true;
  pio_sm_set_enabled(capturePIO, sm_Capture, true);
  return true;
}

bool la_stream_take(la_analysis_block_t *block)
{
  if (!la_stream_running || !block || la_stream_block_outstanding) {
    return false;
  }

  uint64_t completed_chunks =
      la_stream_produced_bytes() / la_config.chunk_bytes;

  if (la_stream_next_chunk >= completed_chunks) {
    return false;
  }

  const uint32_t slot_count = CAPTURE_BUFFER_SIZE / la_config.chunk_bytes;

  if (completed_chunks - la_stream_next_chunk >= slot_count) {
    uint64_t oldest_available = completed_chunks - slot_count + 1;
    la_stream_overruns += (uint32_t)(oldest_available - la_stream_next_chunk);
    la_stream_next_chunk = oldest_available;
  }

  uint32_t offset = (uint32_t)(
      (la_stream_next_chunk % slot_count) * la_config.chunk_bytes);

  block->data = captureBuffer + offset;
  block->bytes = la_config.chunk_bytes;
  block->samples = la_config.chunk_bytes;
  block->sequence = (uint32_t)la_stream_next_chunk;
  block->overruns = la_stream_overruns;
  block->first_sample = la_stream_next_chunk * la_config.chunk_bytes;
  block->device_time_us = time_us_64();
  block->slot = 0;

  la_stream_next_chunk++;
  la_stream_block_outstanding = true;
  return true;
}

void la_stream_release(uint8_t slot)
{
  (void)slot;
  la_stream_block_outstanding = false;
}

void la_stream_stop(void)
{
  if (!la_stream_running) {
    return;
  }

  la_stream_running = false;
  la_stream_stop_hardware();
  la_stream_block_outstanding = false;
}
