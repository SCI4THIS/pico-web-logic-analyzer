#include "bsp/board_api.h"
#include "tinyusb_main_bridge.h"
#include "pico_ws_server/web_socket_server.h"
#include "capture.h"
#include "hardware/gpio.h"
#include <cstring>
#include "logic_analyzer_bridge.h"

static WebSocketServer websocket_server(1);
static uint32_t active_connection = 0;
static bool websocket_connected = false;

static bool is_capture_response_pending = false;
static uint32_t capture_connection = 0;
static const uint8_t test_pins[] = {1};

static bool capture_chain_enabled = false;
static bool capture_chain_pending = false;
static uint32_t stream_connection = 0;
static uint32_t next_stream_ms = 0;
static const uint8_t stream_pins[] = { 1 };

static uint32_t requested_frequency = 1000;
static uint32_t requested_chunk_size = 256;

struct __attribute__((packed)) StreamHeader {
  uint32_t magic;
  uint32_t sequence;
  uint32_t sample_count;
  uint32_t sample_rate;
  uint32_t overruns;
  uint32_t payload_bytes;
  uint64_t first_sample;
  uint64_t device_time_us;
};

static_assert(sizeof(StreamHeader) == 40);

static constexpr uint32_t STREAM_MAGIC = 0x31414C50;
static constexpr size_t MAX_STREAM_CHUNK = 8192;

static uint8_t stream_frame[sizeof(StreamHeader) + MAX_STREAM_CHUNK];
static bool block_pending = false;
static la_capture_chain_block_t pending_block;
bool capture_chain_analysis_enabled = false;


static void websocket_connect(WebSocketServer& server, uint32_t connection_id) {
  active_connection = connection_id;
  websocket_connected = true;
  server.sendMessage(connection_id, "connected");
}

static void websocket_disconnect(WebSocketServer& server, uint32_t connection_id) {
  (void)server;
  (void)connection_id;
  websocket_connected = false;
}

static void start_test_capture(WebSocketServer& server, uint32_t id)
{
  gpio_pull_up(2);
  sleep_us(10);
  bool started = capture::start_simple(1000000, 0, 64, test_pins, 1, 0, true);
  if (started) {
    is_capture_response_pending = true;
    capture_connection = id;
    gpio_pull_up(2);
    server.sendMessage(id, "CAPTURE_STARTED");
    sleep_us(10);
    gpio_pull_down(2);
  } else {
    server.sendMessage(id, "CAPTURE_ERROR");
  }
}

static bool start_capture_chain()
{
  gpio_pull_down(2);
  sleep_us(50);
  bool started = capture::start_simple(1000000, 0, 32, stream_pins, 1, 0, true, capture::Mode::channels8);
  if (started) {
    gpio_pull_up(2);
  }
  return started;
}

static bool send_capture_chain_analysis_block(const la_capture_chain_block_t& block)
{
  if (block.bytes > MAX_STREAM_CHUNK) {
    return false;
  }
  StreamHeader header = {
    STREAM_MAGIC,
    block.sequence,
    block.samples,
    requested_frequency,
    block.overruns,
    block.bytes,
    block.first_sample,
    block.device_time_us,
  };
  memmove(stream_frame, &header, sizeof(header));
  memmove(stream_frame + sizeof(header), block.data, block.bytes);
  return websocket_server.sendMessage(stream_connection, stream_frame, sizeof(header) + block.bytes);
}

static void websocket_message(WebSocketServer& server, uint32_t id, const void *data, size_t length) {
  uint32_t value;
  char command[64];
  size_t count = length < sizeof(command) - 1 ? length : sizeof(command) - 1;

#define COMMAND_IS(s) \
  (length == sizeof(s) - 1 && memcmp(data, s, sizeof(s) - 1) == 0)

  if (COMMAND_IS("STATUS")) {
    server.sendMessage(id, "SNAFU!");
    return;
  }
  if (COMMAND_IS("CAPTURE_TEST")) {
    start_test_capture(server, id);
    return;
  }
  if (COMMAND_IS("CAPTURE_CHAIN_START")) {
    capture_chain_enabled = true;
    stream_connection = id;
    next_stream_ms = 0;
    server.sendMessage(id, "CAPTURE_CHAIN_STARTED");
    return;
  }
  if (COMMAND_IS("CAPTURE_CHAIN_STOP")) {
    capture_chain_enabled = false;
    capture::stop();
    gpio_disable_pulls(2);
    server.sendMessage(id, "CAPTURE_CHAIN_STOPPED");
    return;
  }
  if (COMMAND_IS("PIN_TEST")) {
    server.sendMessage(id, gpio_get(3) ? "GPIO:HIGH" : "GPIO:LOW");
    return;
  }
  if (COMMAND_IS("CAPTURE_CHAIN_ANALYSIS_START")) {
    if (!la_capture_chain_configure(requested_frequency, requested_chunk_size, stream_pins, 1)) {
      server.sendMessage(id, "CONFIG_ERROR");
      return;
    }
    if (!la_capture_chain_start()) {
      server.sendMessage(id, "START_ERROR");
      return;
    }
    stream_connection = id;
    capture_chain_analysis_enabled = true;
    block_pending = false;
    server.sendMessage(id, "CAPTURE_CHAIN_ANALYSIS_STARTED");
    return;
  }
  if (COMMAND_IS("CAPTURE_CHAIN_ANALYSIS_STOP")) {
    la_capture_chain_stop();
    capture_chain_analysis_enabled = false;
    block_pending = false;
    server.sendMessage(id, "CAPTURE_CHAIN_ANALYSIS_STOPPED");
    return;
  }
  memmove(command, data, count);
  command[count] = '\0';
  if (sscanf(command, "SET_FREQ %lu", &value) == 1) {
    requested_frequency = value;
    server.sendMessage(id, "OK");
    return;
  }
  if (sscanf(command, "SET_CHUNK_SIZE %lu", &value) == 1) {
    requested_chunk_size = value;
    server.sendMessage(id, "OK");
    return;
  }
  server.sendMessage(id, data, length);
#undef COMMAND_IS
}

bool websocket_init(void) {
  websocket_server.setConnectCallback(websocket_connect);
  websocket_server.setCloseCallback(websocket_disconnect);
  websocket_server.setMessageCallback(websocket_message);
  websocket_server.setTcpNoDelay(true);
  return websocket_server.startListening(80);
}

int main(void) {
  tinyusb_network_init();

  board_led_write(true);
  if (!websocket_init()) {
    printf("Websocket initialization failed\n");
  }
  board_led_write(false);

  while (1) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    tinyusb_network_poll();
    websocket_server.popMessages();
    if (is_capture_response_pending && !capture::busy()) {
      auto result = capture::result();
      size_t offset = result.first_sample * result.bytes_per_sample;
      size_t length = result.samples * result.bytes_per_sample;
      websocket_server.sendMessage(capture_connection, result.buffer + offset, length);
      gpio_disable_pulls(2);
      is_capture_response_pending = false;
    }
    if (capture_chain_pending && !capture::busy()) {
#define STREAM_CAPTURE_DEBUG 0
#if STREAM_CAPTURE_DEBUG
      uint32_t raw_high = la_raw_high_count();
#endif
      auto result = capture::result();
#if STREAM_CAPTURE_DEBUG
      char debug[160];
      snprintf(debug, sizeof(debug), "samples=%lu first=%lu widtdh=%u gpio=%u firstByte=%u, rawHigh=%lu",
               result.first_sample, result.bytes_per_sample, gpio_get(3), result.buffer[result.first_sample], raw_high);
      websocket_server.sendMessage(stream_connection, debug);
#endif
      size_t offset = result.first_sample * result.bytes_per_sample;
      websocket_server.sendMessage(stream_connection, result.buffer + offset, 32);
      capture_chain_pending = false;
      next_stream_ms = now + 50;
    }
    if (capture_chain_enabled && !capture_chain_pending && now >= next_stream_ms) {
      capture_chain_pending = start_capture_chain();
    }
    if (capture_chain_analysis_enabled && !block_pending) {
      block_pending = la_capture_chain_take(&pending_block);
    }
    if (block_pending) {
      if (send_capture_chain_analysis_block(pending_block)) {
        la_capture_chain_release(pending_block.slot);
	block_pending = false;
      }
    }
  }
}
