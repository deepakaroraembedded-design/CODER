// Hidden acceptance test: concurrency safety under TSan (design section
// 17.2). Built with -fsanitize=thread; any reported race fails the step
// before the counts line is printed, so the harness counts it as failed.
//
// The buffer is sized to hold every item so overwrite-on-full (M1 semantics)
// never kicks in: the consumer sees exactly the producer's sequence, so both
// ordering and race freedom are checked in one bounded run.
#include <atomic>
#include <cstdio>
#include <thread>

#include "ring_buffer.hpp"

int main() {
  constexpr int kItems = 20000;
  cf_golden::RingBuffer<int, kItems> buffer;
  std::atomic<bool> done{false};
  int mismatches = 0;
  int consumed = 0;

  std::thread producer([&buffer, &done]() {
    for (int i = 0; i < kItems; ++i) {
      buffer.push(i);
    }
    done.store(true, std::memory_order_release);
  });
  std::thread consumer([&buffer, &done, &consumed, &mismatches]() {
    int expected = 0;
    while (expected < kItems) {
      auto value = buffer.pop();
      if (!value.has_value()) {
        if (done.load(std::memory_order_acquire) && buffer.empty()) {
          break;  // the producer finished and nothing is left to drain
        }
        continue;
      }
      if (*value != expected) {
        ++mismatches;
      }
      ++expected;
      ++consumed;
    }
  });
  producer.join();
  consumer.join();

  const bool ok = mismatches == 0 && consumed == kItems;
  const int passed = ok ? 1 : 0;
  std::printf("{\"tests\": 1, \"passed\": %d}\n", passed);
  return passed == 1 ? 0 : 1;
}
