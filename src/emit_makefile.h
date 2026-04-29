#pragma once

#include <string>
#include <vector>

namespace tp2cc {

struct EmittedBuildManifest {
  std::vector<std::string> cc_sources;
  std::vector<std::string> headers;
  std::string tp2cc_root;
  std::string program_name;
};

std::string emit_makefile(const EmittedBuildManifest& manifest);

}  // namespace tp2cc
