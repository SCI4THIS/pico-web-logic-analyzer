#include "LogicAnalyzer_Capture.c"

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
