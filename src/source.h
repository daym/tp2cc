#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tp2cc {

struct SourceFile {
  std::string path;
  std::string contents;

  SourceFile() = default;
  SourceFile(std::string path_in, std::string contents_in)
      : path(std::move(path_in)), contents(std::move(contents_in)) {}

  static std::shared_ptr<SourceFile> load(const std::filesystem::path& p);
};

struct Location {
  std::shared_ptr<const SourceFile> file;
  uint32_t line = 0;
  uint32_t col = 0;

  std::string to_string() const;
};

}  // namespace tp2cc
