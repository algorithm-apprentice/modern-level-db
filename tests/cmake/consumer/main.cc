#include "modern_leveldb/db.h"

int main() {
  modern_leveldb::Options options;
  options.block_restart_interval = 0;
  const auto result = modern_leveldb::Database::Open(options, "unused");
  return result.has_value() ? 1 : 0;
}
