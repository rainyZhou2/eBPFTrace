#pragma once

#include <memory>

#include "platform/config.h"
#include "storage/storage.h"

namespace etrace::storage {

// open builds the configured driver: "memory" or "clickhouse". Adding a
// driver means implementing Store and adding it here.
std::unique_ptr<Store> open(const config::Plugin& cfg);

}  // namespace etrace::storage
