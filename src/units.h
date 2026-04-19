#pragma once

// Multi-unit compilation context.
//
// `UnitGraph` is responsible for:
//   - finding all `.pas`/`.pp` files under a set of root directories
//   - parsing each as a Pascal compilation unit
//   - collecting their `uses` dependencies (interface + implementation)
//   - producing a topological order for emission
//
// Unit names are case-insensitive (Pascal convention). We store lowercased
// names as canonical keys.

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ast.h"

namespace p2cc {

struct ParsedUnit {
  std::string name;                    // lowercased canonical name
  std::filesystem::path path;
  std::unique_ptr<ast::UnitNode> ast;  // null on parse failure
  bool ok = false;                     // true if parsed without errors
};

class UnitGraph {
 public:
  UnitGraph();

  // Add a search root. Called before `discover()`.
  void add_search_root(std::filesystem::path p);

  // Predefine a preprocessor symbol used when parsing each unit.
  void define(std::string name);

  // Skip a file (by substring match of its path). Used for build-utilities
  // like `tokendat.pas` that aren't part of the compiler proper.
  void skip_path_containing(std::string needle);

  // Find all candidate .pas/.pp files under the roots and parse each into a
  // ParsedUnit. Returns the number of parse errors across all files.
  int discover();

  // Parse a single program/unit file and recursively discover only the units
  // reachable through its `uses` graph. Search roots are still used to locate
  // referenced units by filename.
  int discover_from_entry(std::filesystem::path entry_path);

  // After discover(), compute a topological order so that each unit's
  // dependencies come before it. Returns the order as a sequence of unit
  // names; also returns any cycles as pairs of (from, to).
  struct TopoResult {
    std::vector<std::string> order;                  // lowercased names
    std::vector<std::pair<std::string, std::string>> cycle_edges;
  };
  TopoResult topo_sort() const;

  // Lookup a parsed unit by (lowercased) name.
  const ParsedUnit* lookup(std::string_view name) const;

  // Iteration over all parsed units.
  const std::unordered_map<std::string, ParsedUnit>& units() const {
    return units_;
  }

 private:
  std::vector<std::filesystem::path> roots_;
  std::vector<std::string> defines_;
  std::vector<std::string> skip_needles_;

  // Map lowercased unit name -> ParsedUnit.
  std::unordered_map<std::string, ParsedUnit> units_;
  std::unordered_map<std::string, std::vector<std::filesystem::path>>
      unit_path_index_;
  bool unit_path_index_ready_ = false;

  bool skipped(const std::string& path) const;
  static std::string to_lower(std::string_view s);
  void build_unit_path_index();
  std::filesystem::path find_unit_path(std::string_view name);
  int parse_recursive(const std::filesystem::path& path);
};

}  // namespace p2cc
