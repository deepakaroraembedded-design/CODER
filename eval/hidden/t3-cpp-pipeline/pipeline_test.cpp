// Hidden acceptance test: message v2 wire format, channel FIFO and the
// producer/consumer pipeline (design section 17.2). Built against the final
// integration state, after M4 changed the shared message interface.
#include <cstdio>
#include <string>
#include <vector>

#include "pipeline/channel.hpp"
#include "pipeline/consumer.hpp"
#include "pipeline/message.hpp"
#include "pipeline/producer.hpp"

int main() {
  int checks = 0;
  int passed = 0;
  const auto check = [&](bool ok) {
    ++checks;
    if (ok) {
      ++passed;
    }
  };

  // The v2 format carries the version: "id:version:payload".
  cf_golden::Message message;
  message.id = 7;
  message.payload = "hello";
  message.version = 2;
  const std::string encoded = cf_golden::encode(message);
  check(encoded == "7:2:hello");
  const auto decoded = cf_golden::decode(encoded);
  check(decoded.has_value());
  if (decoded.has_value()) {
    check(decoded->id == 7 && decoded->version == 2 && decoded->payload == "hello");
  }

  // The legacy v1 format still parses with version 1.
  const auto legacy = cf_golden::decode("3:old");
  check(legacy.has_value());
  if (legacy.has_value()) {
    check(legacy->id == 3 && legacy->version == 1 && legacy->payload == "old");
  }

  // Malformed input is rejected, not guessed.
  check(!cf_golden::decode("no-separator").has_value());

  // Producer -> channel -> consumer, FIFO.
  cf_golden::Channel channel;
  check(channel.empty());
  cf_golden::Producer producer(channel);
  producer.submit(1, "alpha");
  producer.submit(2, "beta");
  check(!channel.empty());
  cf_golden::Consumer consumer(channel);
  const std::vector<cf_golden::Message> messages = consumer.drain();
  check(messages.size() == 2);
  if (messages.size() == 2) {
    check(messages[0].id == 1 && messages[0].payload == "alpha");
    check(messages[1].id == 2 && messages[1].payload == "beta");
  }
  check(channel.empty());

  std::printf("{\"tests\": %d, \"passed\": %d}\n", checks, passed);
  return passed == checks ? 0 : 1;
}
