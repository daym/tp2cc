#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tp2cc {

struct SourceFile {
  std::string path;
  std::string contents;

  static std::shared_ptr<SourceFile> load(const std::filesystem::path& p);
};

struct Location {
  std::shared_ptr<const SourceFile> file;
  uint32_t line = 0;
  uint32_t col = 0;

  std::string to_string() const;
};

}  // namespace tp2cc
