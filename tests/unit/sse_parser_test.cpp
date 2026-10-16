// Unit tests for the SSE parser (design section 6.6.2, scenarios H1/H7).
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cf/agents/opencode_client.hpp"

namespace {

using cf::agents::OpencodeEvent;
using cf::agents::SseParser;

TEST(SseParserTest, SingleEvent) {
  std::vector<OpencodeEvent> events;
  SseParser parser([&](const OpencodeEvent& e) { events.push_back(e); });
  parser.feed("data: {\"id\":\"evt_1\",\"type\":\"session.idle\",\"properties\":{\"sessionID\":\"ses_1\"}}\n\n");

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].id, "evt_1");
  EXPECT_EQ(events[0].type, "session.idle");
  EXPECT_EQ(events[0].properties["sessionID"], "ses_1");
}

TEST(SseParserTest, ChunkBoundaries) {
  std::vector<OpencodeEvent> events;
  SseParser parser([&](const OpencodeEvent& e) { events.push_back(e); });
  const std::string payload =
      "data: {\"type\":\"permission.asked\",\"properties\":{\"id\":\"per_1\"}}\n\n";
  for (char c : payload) {
    parser.feed(std::string_view(&c, 1));
  }
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, "permission.asked");
  EXPECT_EQ(events[0].properties["id"], "per_1");
}

TEST(SseParserTest, MultiLineData) {
  std::vector<OpencodeEvent> events;
  SseParser parser([&](const OpencodeEvent& e) { events.push_back(e); });
  parser.feed("data: {\"type\":\"x\",\ndata: \"properties\":{}}\n\n");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, "x");
}

TEST(SseParserTest, CommentsAndEventField) {
  std::vector<OpencodeEvent> events;
  SseParser parser([&](const OpencodeEvent& e) { events.push_back(e); });
  parser.feed(": heartbeat\n\n");
  EXPECT_TRUE(events.empty());
  parser.feed("event: custom\ndata: {\"type\":\"a\",\"properties\":{}}\n\n");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, "a");
}

TEST(SseParserTest, InvalidJsonBecomesParseError) {
  std::vector<OpencodeEvent> events;
  SseParser parser([&](const OpencodeEvent& e) { events.push_back(e); });
  parser.feed("data: {not json\n\n");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, "spike.parse_error");
}

TEST(SseParserTest, GlobalEventPayloadUnwrapped) {
  std::vector<OpencodeEvent> events;
  SseParser parser([&](const OpencodeEvent& e) { events.push_back(e); });
  parser.feed(
      "data: {\"directory\":\"/x\",\"payload\":{\"id\":\"evt_9\","
      "\"type\":\"session.idle\",\"properties\":{\"sessionID\":\"ses_9\"}}}\n\n");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, "session.idle");
  EXPECT_EQ(events[0].id, "evt_9");
  EXPECT_EQ(events[0].properties["sessionID"], "ses_9");
}

}  // namespace
