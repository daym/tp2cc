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

  static std::unique_ptr<SourceFile> load(const std::filesystem::path& p);
};

struct Location {
  // Non-owning.  The SourceFile must outlive every Location that
  // references it -- Location is copied freely into AST nodes,
  // TypeRegistry entries, and deferred diagnostics consumed at emit
  // time.  Ownership lives in ParsedUnit::sources (populated via
  // Lexer::release_sources() in UnitGraph::parse_recursive); do NOT
  // store a Location that outlives its ParsedUnit.
  const SourceFile* file = nullptr;
  uint32_t line = 0;
  uint32_t col = 0;

  std::string to_string() const;
};

}  // namespace tp2cc
