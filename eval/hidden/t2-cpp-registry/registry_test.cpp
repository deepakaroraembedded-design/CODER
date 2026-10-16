// Hidden acceptance test: agent registry with observer callbacks (design
// section 17.2).
//
// Never shown to the agents. The plan pins the interface: header-only
// `include/registry.hpp`, namespace cf_golden, Registry<Key, Value> with
// bool add(const Key&, Value), std::optional<Value> lookup(const Key&) const,
// bool remove(const Key&), std::size_t size() const, std::vector<Key> keys()
// const in registration order, and set_observer(std::function<void(const
// Key&, bool added)>), called once per successful mutation.
#include <cstdio>
#include <string>
#include <vector>

#include "registry.hpp"

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
  // Duplicate registrations are rejected.
  {
    cf_golden::Registry<std::string, int> registry;
    check(registry.add("a", 1));
    check(!registry.add("a", 2));
    check(registry.size() == 1);
    const auto value = registry.lookup("a");
    check(value.has_value() && *value == 1);
  }

  // Lookup returns entries; keys() keeps registration order.
  {
    cf_golden::Registry<std::string, int> registry;
    registry.add("first", 1);
    registry.add("second", 2);
    registry.add("third", 3);
    check(registry.lookup("second").value_or(0) == 2);
    check(!registry.lookup("missing").has_value());
    check((registry.keys() == std::vector<std::string>{"first", "second", "third"}));
    check(registry.remove("second"));
    check(!registry.remove("second"));
    check((registry.keys() == std::vector<std::string>{"first", "third"}));
  }

  // Callbacks fire exactly once per successful mutation.
  {
    cf_golden::Registry<std::string, int> registry;
    std::vector<std::pair<std::string, bool>> events;
    registry.set_observer([&events](const std::string& key, bool added) {
      events.emplace_back(key, added);
    });
    registry.add("a", 1);
    registry.add("a", 9);  // rejected: no event
    registry.remove("a");
    registry.remove("a");  // absent: no event
    check(events.size() == 2);
    check(events.size() == 2 && events[0].first == "a" && events[0].second);
    check(events.size() == 2 && events[1].first == "a" && !events[1].second);
  }

  // Unregistering during iteration does not invalidate the iteration.
  {
    cf_golden::Registry<std::string, int> registry;
    registry.add("a", 1);
    registry.add("b", 2);
    registry.add("c", 3);
    int seen = 0;
    for (const auto& key : registry.keys()) {
      ++seen;
      registry.remove(key);
    }
    check(seen == 3);
    check(registry.size() == 0);
  }

  std::printf("{\"tests\": %d, \"passed\": %d}\n", checks, passed);
  return passed == checks ? 0 : 1;
}
