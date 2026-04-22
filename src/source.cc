#include "source.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace tp2cc {

std::shared_ptr<SourceFile> SourceFile::load(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return nullptr;
  std::ostringstream ss;
  ss << f.rdbuf();
  auto sf = std::make_shared<SourceFile>();
  sf->path = p.string();
  sf->contents = ss.str();
  return sf;
}

std::string Location::to_string() const {
  char buf[64];
  std::snprintf(buf, sizeof(buf), ":%u:%u", line, col);
  return (file ? file->path : std::string("<no-file>")) + buf;
}

}  // namespace tp2cc
