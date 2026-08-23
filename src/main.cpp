#include "bsp/board_api.h"
#include "tinyusb_main_bridge.h"
#include "pico_ws_server/web_socket_server.h"
#include "hardware/gpio.h"
#include <cstring>
#include "logic_analyzer_bridge.h"

static WebSocketServer websocket_server(1);
static uint32_t active_connection = 0;
static bool websocket_connected = false;

static bool single_capture_pending = false;
static uint32_t single_capture_connection = 0;

static bool capture_chain_enabled = false;
static bool capture_chain_pending = false;
static uint32_t stream_connection = 0;
static uint32_t next_stream_ms = 0;

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
static constexpr size_t MAX_STREAM_CHUNK = LA_MAX_CHUNK_BYTES;

static uint8_t stream_frame[sizeof(StreamHeader) + MAX_STREAM_CHUNK];
static bool block_pending = false;
static la_analysis_block_t pending_analysis_block;
bool capture_chain_analysis_enabled = false;

enum class StreamMode {
  stopped,
  raw,
  analysis,
};

static StreamMode stream_mode = StreamMode::stopped;
static bool stream_block_pending = false;

static bool is_busy()
{
  return la_is_busy() || single_capture_pending ||
         capture_chain_enabled || capture_chain_pending;
}

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

static void start_single_capture(WebSocketServer& server, uint32_t id)
{
  gpio_pull_up(LA_TRIGGER_GPIO);
  sleep_us(10);
  bool started = la_single_capture_start(0, 64, LA_TRIGGER_CHANNEL, true);
  if (started) {
    single_capture_pending = true;
    single_capture_connection = id;
    gpio_pull_up(LA_TRIGGER_GPIO);
    server.sendMessage(id, "SINGLE_CAPTURE_STARTED");
    sleep_us(10);
    gpio_pull_down(LA_TRIGGER_GPIO);
  } else {
    server.sendMessage(id, "SINGLE_CAPTURE_ERROR");
  }
}

static bool start_capture_chain()
{
  gpio_pull_down(LA_TRIGGER_GPIO);
  sleep_us(50);
  bool started = la_single_capture_start(0, 32, LA_TRIGGER_CHANNEL, true);
  if (started) {
    gpio_pull_up(LA_TRIGGER_GPIO);
  }
  return started;
}

static bool send_analysis_block(const la_analysis_block_t& block)
{
  if (block.bytes > MAX_STREAM_CHUNK) {
    return false;
  }
  StreamHeader header = {
    STREAM_MAGIC,
    block.sequence,
    block.samples,
    la_configure_frequency(NULL),
    block.overruns,
    block.bytes,
    block.first_sample,
    block.device_time_us,
  };
  memmove(stream_frame, &header, sizeof(header));
  memmove(stream_frame + sizeof(header), block.data, block.bytes);
  return websocket_server.sendMessage(stream_connection, stream_frame, sizeof(header) + block.bytes);
}

static bool start_stream(WebSocketServer& server, uint32_t id, StreamMode mode)
{
  if (is_busy()) {
    server.sendMessage(id, "CAPTURE_BUSY");
    return false;
  }

  if (!la_stream_start()) {
    server.sendMessage(id, "START_ERROR");
    return false;
  }

  stream_connection = id;
  stream_mode = mode;
  stream_block_pending = false;
  return true;
}

static void stop_stream(void)
{
  la_stream_stop();
  stream_mode = StreamMode::stopped;
  stream_block_pending = false;
}

static void websocket_message(WebSocketServer& server, uint32_t id, const void *data, size_t length) {
  uint32_t value;
  char command[64];
  size_t count = length < sizeof(command) - 1 ? length : sizeof(command) - 1;

#define COMMAND_IS(s) \
  (length == sizeof(s) - 1 && memcmp(data, s, sizeof(s) - 1) == 0)

  if (COMMAND_IS("STATUS")) {
    server.sendMessage(id, "We're all find here now, thank you.. How are you?");
    return;
  }
  if (COMMAND_IS("SINGLE_CAPTURE")) {
    if (is_busy()) {
      server.sendMessage(id, "CAPTURE_BUSY");
      return;
    }
    start_single_capture(server, id);
    return;
  }
  if (COMMAND_IS("CAPTURE_CHAIN_START")) {
    if (is_busy()) {
      server.sendMessage(id, "CAPTURE_BUSY");
      return;
    }
    capture_chain_enabled = true;
    stream_connection = id;
    next_stream_ms = 0;
    server.sendMessage(id, "CAPTURE_CHAIN_STARTED");
    return;
  }
  if (COMMAND_IS("CAPTURE_CHAIN_STOP")) {
    capture_chain_enabled = false;
    la_single_capture_stop();
    gpio_disable_pulls(LA_TRIGGER_GPIO);
    server.sendMessage(id, "CAPTURE_CHAIN_STOPPED");
    return;
  }
  if (COMMAND_IS("PIN_TEST")) {
    server.sendMessage(id, gpio_get(3) ? "GPIO:HIGH" : "GPIO:LOW");
    return;
  }
  if (COMMAND_IS("CAPTURE_CHAIN_ANALYSIS_START")) {
    if (is_busy()) {
      server.sendMessage(id, "CAPTURE_BUSY");
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
  if (COMMAND_IS("STREAM_START")) {
    if (start_stream(server, id, StreamMode::raw)) {
      server.sendMessage(id, "STREAM_STARTED");
    }
    return;
  }
  if (COMMAND_IS("STREAM_STOP")) {
    stop_stream();
    server.sendMessage(id, "STREAM_STOPPED");
    return;
  }
  if (COMMAND_IS("STREAM_ANALYSIS_START")) {
    if (start_stream(server, id, StreamMode::analysis)) {
      server.sendMessage(id, "STREAM_ANALYSIS_STARTED");
    }
    return;
  }
  if (COMMAND_IS("STREAM_ANALYSIS_STOP")) {
    stop_stream();
    server.sendMessage(id, "STREAM_ANALYSIS_STOPPED");
    return;
  }
  memmove(command, data, count);
  command[count] = '\0';
  if (sscanf(command, "SET_FREQ %lu", &value) == 1) {
    value = la_configure_frequency(&value);
    snprintf(command, sizeof(command), "FREQ %lu", value);
    server.sendMessage(id, command);
    return;
  }
  if (sscanf(command, "SET_CHUNK_SIZE %lu", &value) == 1) {
    value = la_configure_chunk_bytes(&value);
    snprintf(command, sizeof(command), "CHUNKS_SIZE %lu", value);
    server.sendMessage(id, command);
    return;
  }
  if (sscanf(command, "SET_CHANNEL_MASK %lx", &value) == 1) {
    value = la_configure_channel_mask(&value);
    snprintf(command, sizeof(command), "CHANNEL_MASK %06lx", value);
    server.sendMessage(id, command);
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

void single_capture_tasks()
{
  if (single_capture_pending && !la_single_capture_is_running()) {
    la_capture_result_t result;
    if (la_single_capture_get_result(&result)) {
      size_t offset = result.first_sample * result.bytes_per_sample;
      size_t length = result.samples * result.bytes_per_sample;
      websocket_server.sendMessage(
          single_capture_connection, result.buffer + offset, length);
    } else {
      websocket_server.sendMessage(
          single_capture_connection, "SINGLE_CAPTURE_ERROR");
    }

    la_single_capture_release();
    gpio_disable_pulls(LA_TRIGGER_GPIO);
    single_capture_pending = false;
  }
}

void capture_chain_tasks()
{
  uint32_t now = to_ms_since_boot(get_absolute_time());
  if (capture_chain_pending && !la_single_capture_is_running()) {
#define STREAM_CAPTURE_DEBUG 0
#if STREAM_CAPTURE_DEBUG
    uint32_t raw_high = la_raw_high_count();
#endif
    la_capture_result_t result;
    if (!la_single_capture_get_result(&result)) {
      la_single_capture_release();
      capture_chain_pending = false;
      return;
    }
#if STREAM_CAPTURE_DEBUG
    char debug[160];
    snprintf(debug, sizeof(debug), "samples=%lu first=%lu width=%u gpio=%u firstByte=%u, rawHigh=%lu",
             result.first_sample, result.bytes_per_sample, gpio_get(3), result.buffer[result.first_sample], raw_high);
    websocket_server.sendMessage(stream_connection, debug);
#endif
    size_t offset = result.first_sample * result.bytes_per_sample;
    websocket_server.sendMessage(stream_connection, result.buffer + offset, 32);
    la_single_capture_release();
    capture_chain_pending = false;
    next_stream_ms = now + 50;
  }
  if (capture_chain_enabled && !capture_chain_pending && now >= next_stream_ms) {
    capture_chain_pending = start_capture_chain();
  }
  if (capture_chain_analysis_enabled && !block_pending) {
    block_pending = la_capture_chain_take(&pending_analysis_block);
  }
  if (block_pending) {
    if (send_analysis_block(pending_analysis_block)) {
      la_capture_chain_release(pending_analysis_block.slot);
      block_pending = false;
    }
  }
}

void stream_tasks()
{
  if (stream_mode != StreamMode::stopped && !stream_block_pending) {
    stream_block_pending = la_stream_take(&pending_analysis_block);
  }
  if (stream_block_pending) {
    bool sent = stream_mode == StreamMode::analysis
        ? send_analysis_block(pending_analysis_block)
        : websocket_server.sendMessage(
              stream_connection,
              pending_analysis_block.data,
              pending_analysis_block.bytes);

    if (sent) {
      la_stream_release(pending_analysis_block.slot);
      stream_block_pending = false;
    }
  }
}

int main(void) {
  tinyusb_network_init();

  board_led_write(true);
  if (!websocket_init()) {
    printf("Websocket initialization failed\n");
  }
  board_led_write(false);

  while (1) {
    tinyusb_network_poll();
    websocket_server.popMessages();
    single_capture_tasks();
    capture_chain_tasks();
    stream_tasks();
  }
}
