#pragma once

#include <string_view>

#include "source.h"

namespace tp2cc {

// Plain global error reporting. No engine, no levels needed yet.
void report_error(Location where, std::string_view msg);
void report_warning(Location where, std::string_view msg);

int error_count();

}  // namespace tp2cc
