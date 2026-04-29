#pragma once

#include <string>
#include <vector>

namespace tp2cc {

struct EmittedBuildManifest {
  std::vector<std::string> cc_sources;
  std::vector<std::string> headers;
  std::vector<std::string> pas_sources;
  std::string tp2cc_program;
  std::vector<std::string> include_dirs;
  std::vector<std::string> tp2cc_args;
  std::string program_name;
};

std::string emit_makefile(const EmittedBuildManifest& manifest);

}  // namespace tp2cc
