// Hidden acceptance test: fixed-capacity ring buffer (design section 17.2).
//
// Never shown to the agents. The plan pins the interface: header-only
// `include/ring_buffer.hpp`, namespace cf_golden, RingBuffer<T, Capacity>
// with void push(const T&) / void push(T&&), std::optional<T> pop(),
// size(), empty(), full(); a full buffer drops the oldest element.
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "ring_buffer.hpp"

namespace {

int checks = 0;
int passed = 0;

void check(bool condition) {
  ++checks;
  if (condition) {
    ++passed;
  }
}

}  // namespace

int main() {
  // FIFO order across wraparound.
  {
    cf_golden::RingBuffer<int, 3> buffer;
    buffer.push(1);
    buffer.push(2);
    buffer.push(3);
    buffer.pop();  // drop 1
    buffer.push(4);
    const auto second = buffer.pop();
    const auto third = buffer.pop();
    const auto fourth = buffer.pop();
    check(second.has_value() && *second == 2);
    check(third.has_value() && *third == 3);
    check(fourth.has_value() && *fourth == 4);
  }

  // Overwrite-on-full drops the oldest element.
  {
    cf_golden::RingBuffer<int, 2> buffer;
    buffer.push(10);
    buffer.push(20);
    buffer.push(30);
    const auto first = buffer.pop();
    const auto next = buffer.pop();
    check(first.has_value() && *first == 20);
    check(next.has_value() && *next == 30);
  }

  // Size, empty and full transitions.
  {
    cf_golden::RingBuffer<int, 2> buffer;
    check(buffer.empty());
    check(!buffer.full());
    buffer.push(1);
    check(buffer.size() == 1 && !buffer.empty() && !buffer.full());
    buffer.push(2);
    check(buffer.size() == 2 && buffer.full());
    buffer.pop();
    check(buffer.size() == 1 && !buffer.full());
  }

  // Move-only element types.
  {
    cf_golden::RingBuffer<std::unique_ptr<int>, 2> buffer;
    buffer.push(std::make_unique<int>(7));
    buffer.push(std::make_unique<int>(8));
    auto first = buffer.pop();
    check(first.has_value() && first->has_value() && **first == 7);
  }

  // Pop on an empty buffer returns nullopt.
  {
    cf_golden::RingBuffer<std::string, 1> buffer;
    check(!buffer.pop().has_value());
  }

  std::printf("{\"tests\": %d, \"passed\": %d}\n", checks, passed);
  return passed == checks ? 0 : 1;
}
