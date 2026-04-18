#include "diag.h"

#include <cstdio>

namespace p2cc {

static int g_errors = 0;

void report_error(Location where, std::string_view msg) {
  std::fprintf(stderr, "%s: error: %.*s\n",
               where.to_string().c_str(),
               static_cast<int>(msg.size()), msg.data());
  ++g_errors;
}

void report_warning(Location where, std::string_view msg) {
  std::fprintf(stderr, "%s: warning: %.*s\n",
               where.to_string().c_str(),
               static_cast<int>(msg.size()), msg.data());
}

int error_count() { return g_errors; }

}  // namespace p2cc
