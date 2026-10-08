#include "support/crash_observation.h"

#include <gtest/gtest.h>

#include <string>

namespace modern_leveldb::test_support {
namespace {

std::string CompleteObservation() {
  std::string records;
  for (unsigned index = 0; index < 8; ++index) {
    records += "ack " + std::to_string(index) + "\n";
  }
  return records + "ready\n";
}

TEST(CrashObservationTest, AcceptsOnlyTheCompleteOrderedProtocolAcrossReadChunks) {
  CrashObservation observation;
  const std::string records = CompleteObservation();
  for (const char character : records) {
    ASSERT_TRUE(observation.Append(std::string_view(&character, 1)).has_value());
  }
  EXPECT_TRUE(observation.ready());
  EXPECT_TRUE(observation.Finish().has_value());
}

TEST(CrashObservationTest, RejectsMissingDuplicateUnknownAndUnterminatedEvidence) {
  for (const std::string input :
       {"", "ready\n", "ack 1\n", "ack 0\nack 0\n", "unknown\n", "ack 0"}) {
    CrashObservation observation;
    const auto appended = observation.Append(input);
    EXPECT_FALSE(appended.has_value() && observation.Finish().has_value()) << input;
  }
}

TEST(CrashObservationTest, RejectsInvalidTailAfterAnOtherwiseCompletePrefix) {
  CrashObservation observation;
  ASSERT_TRUE(observation.Append(CompleteObservation()).has_value());
  EXPECT_TRUE(observation.ready());
  EXPECT_FALSE(observation.Append("bad\n").has_value());
  EXPECT_FALSE(observation.Finish().has_value());
}

TEST(CrashObservationTest, OversizeAndFirstErrorsCannotBecomeSuccess) {
  CrashObservation observation;
  const auto rejected = observation.Append(std::string(4'097, 'x'));
  ASSERT_FALSE(rejected.has_value());
  EXPECT_FALSE(observation.Append(CompleteObservation()).has_value());
  EXPECT_FALSE(observation.Finish().has_value());
}

}  // namespace
}  // namespace modern_leveldb::test_support
