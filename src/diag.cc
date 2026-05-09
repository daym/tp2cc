#include "diag.h"

#include <cstdio>

namespace tp2cc {

static int g_errors = 0;
static int g_warnings = 0;

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
  ++g_warnings;
}

int error_count() { return g_errors; }
int warning_count() { return g_warnings; }

}  // namespace tp2cc
