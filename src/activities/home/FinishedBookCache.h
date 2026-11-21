#pragma once

#include <cstdint>

// Remembers the finished state of recently drawn books. Keep the current
// frame's hits and newly resolved icons while replacing older entries.
class FinishedBookCache {
 public:
  static constexpr uint8_t CAPACITY = 32;

  void beginFrame() { visible = 0; }

  bool lookup(const uint32_t key, bool& finished) {
    for (uint8_t i = 0; i < count; ++i) {
      if (keys[i] == key) {
        visible |= uint32_t{1} << i;
        finished = (bits >> i) & 1u;
        return true;
      }
    }
    return false;
  }

  void store(const uint32_t key, const bool finished) {
    for (uint8_t i = 0; i < count; ++i) {
      if (keys[i] == key) {
        writeFinished(i, finished);
        return;
      }
    }
    uint8_t slot = next;
    if (count < CAPACITY) {
      slot = count++;
    } else {
      const uint32_t available = ~visible;
      if (available == 0) return;
      const uint32_t fromNext = available & (UINT32_MAX << next);
      slot = static_cast<uint8_t>(__builtin_ctz(fromNext ? fromNext : available));
      next = static_cast<uint8_t>((slot + 1) % CAPACITY);
    }
    keys[slot] = key;
    visible |= uint32_t{1} << slot;
    writeFinished(slot, finished);
  }

  void clear() {
    count = 0;
    next = 0;
    bits = 0;
    visible = 0;
  }

 private:
  void writeFinished(const uint8_t slot, const bool finished) {
    if (finished) {
      bits |= uint32_t{1} << slot;
    } else {
      bits &= ~(uint32_t{1} << slot);
    }
  }

  uint32_t keys[CAPACITY]{};
  uint32_t bits = 0;
  uint32_t visible = 0;
  uint8_t count = 0;
  uint8_t next = 0;
};
