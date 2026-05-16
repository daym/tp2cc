#include "units.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "diag.h"
#include "lexer.h"
#include "parser.h"

namespace fs = std::filesystem;

namespace tp2cc {

UnitGraph::UnitGraph() = default;

void UnitGraph::add_search_root(fs::path p) { roots_.push_back(std::move(p)); }

void UnitGraph::add_include_path(fs::path p) {
  include_paths_.push_back(std::move(p));
}

void UnitGraph::define(std::string name) { defines_.push_back(std::move(name)); }

std::string UnitGraph::to_lower(std::string_view s) {
  std::string r(s);
  for (auto& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return r;
}

void UnitGraph::index_unit_search_root(
    const fs::path& root, std::vector<fs::path>& indexed_roots) {
  if (root.empty()) return;
  if (std::find(indexed_roots.begin(), indexed_roots.end(), root) !=
      indexed_roots.end()) {
    return;
  }
  indexed_roots.push_back(root);
  if (!fs::exists(root)) return;
  for (auto& e : fs::directory_iterator(root)) {
    if (!e.is_regular_file()) continue;
    auto ext = e.path().extension();
    if (ext != ".pas" && ext != ".pp") continue;
    std::string stem = to_lower(e.path().stem().string());
    unit_path_index_.try_emplace(stem, e.path());
  }
}

void UnitGraph::build_unit_path_index() {
  if (unit_path_index_ready_) return;
  unit_path_index_.clear();

  std::vector<fs::path> indexed_roots;
  index_unit_search_root(current_dir_, indexed_roots);
  index_unit_search_root(entry_dir_, indexed_roots);
  for (const auto& root : roots_) index_unit_search_root(root, indexed_roots);
  unit_path_index_ready_ = true;
}

fs::path UnitGraph::find_unit_path(std::string_view name) {
  build_unit_path_index();
  auto it = unit_path_index_.find(to_lower(name));
  if (it == unit_path_index_.end()) return {};
  return it->second;
}

std::vector<fs::path> UnitGraph::unit_paths_to_discover(
    const std::vector<std::string>& uses) {
  std::vector<fs::path> paths;
  for (const auto& dep : uses) {
    std::string dep_name = to_lower(dep);
    if (units_.count(dep_name)) continue;
    fs::path dep_path = find_unit_path(dep_name);
    if (dep_path.empty()) continue;  // external RTL / unavailable unit
    paths.push_back(std::move(dep_path));
  }
  return paths;
}

int UnitGraph::parse_recursive(const fs::path& path) {
  auto sf = SourceFile::load(path);
  if (!sf) return 1;

  int errs_before = error_count();
  Lexer lex(std::move(sf), include_paths_);
  for (const auto& d : defines_) lex.define(d);
  lex.set_overflow_check_default(overflow_check_default_);
  lex.set_range_check_default(range_check_default_);
  Parser parser(lex);
  auto node = parser.parse();
  int errs = error_count() - errs_before;

  std::string unit_name = node ? to_lower(node->name) : std::string{};
  if (unit_name.empty()) {
    unit_name = std::string("__prog_") + path.stem().string();
  }
  const bool parsed_ok = errs == 0 && node != nullptr;
  ParsedUnit pu{.name = std::move(unit_name),
                .path = path,
                .ast = std::move(node),
                .ok = parsed_ok};

  auto it = units_.find(pu.name);
  if (it != units_.end()) {
    if (it->second.path != pu.path) {
      report_warning(pu.ast ? pu.ast->loc : Location{},
                     "duplicate unit '" + pu.name + "' at " +
                         path.string() + " (first seen at " +
                         it->second.path.string() + ")");
    }
    return errs > 0 ? errs : 0;
  }

  std::string key = pu.name;
  units_.emplace(key, std::move(pu));
  int errors = errs > 0 ? errs : 0;
  auto key_it = units_.find(key);
  if (key_it == units_.end() || !key_it->second.ok || !key_it->second.ast) {
    return errors > 0 ? errors : 1;
  }

  for (const auto& dep_path :
       unit_paths_to_discover(key_it->second.ast->interface_uses)) {
    errors += parse_recursive(dep_path);
  }
  for (const auto& dep_path :
       unit_paths_to_discover(key_it->second.ast->impl_uses)) {
    errors += parse_recursive(dep_path);
  }
  return errors;
}

int UnitGraph::discover_from_entry(fs::path entry_path) {
  units_.clear();
  unit_path_index_.clear();
  unit_path_index_ready_ = false;
  if (!entry_path.is_absolute()) entry_path = fs::absolute(entry_path);
  current_dir_ = fs::current_path();
  entry_dir_ = entry_path.parent_path();
  return parse_recursive(entry_path);
}

void UnitGraph::add_topo_dependency(
    const std::unordered_map<std::string, ParsedUnit>& units,
    std::unordered_map<std::string, std::unordered_set<std::string>>& deps,
    std::unordered_map<std::string, int>& indeg, const std::string& from,
    std::string_view to) {
  std::string canonical_to = to_lower(to);
  // Topology is only for units parsed from source; external RTL/runtime units
  // are resolved during type registration and emission.
  if (!units.count(canonical_to)) return;
  if (deps[from].insert(canonical_to).second) {
    indeg[from] += 1;
  }
}

UnitGraph::TopoResult UnitGraph::topo_sort() const {
  std::vector<std::string> order;
  std::vector<std::pair<std::string, std::string>> cycle_edges;

  // Build adjacency: for each unit, its (lowercased) uses entries. Edges
  // are reversed for Kahn's algorithm.
  std::unordered_map<std::string, std::unordered_set<std::string>> deps;
  std::unordered_map<std::string, int> indeg;

  for (const auto& [name, pu] : units_) {
    indeg[name] = 0;
  }

  // Pascal semantics: only `interface uses` lists form the unit compile-order
  // graph (and must be acyclic). `implementation uses` may form cycles
  // because implementation bodies are free to reference each other after
  // all interfaces have been established. Programs are the one exception:
  // their `uses` list lives in `impl_uses`, and those dependencies are real.
  for (const auto& [name, pu] : units_) {
    if (!pu.ast) continue;
    for (const auto& u : pu.ast->interface_uses) {
      add_topo_dependency(units_, deps, indeg, name, u);
    }
    if (pu.ast->is_program) {
      for (const auto& u : pu.ast->impl_uses) {
        add_topo_dependency(units_, deps, indeg, name, u);
      }
    }
  }

  // Kahn: repeatedly emit zero-indegree nodes.
  std::vector<std::string> ready;
  for (const auto& [name, d] : indeg) {
    if (d == 0) ready.push_back(name);
  }
  std::sort(ready.begin(), ready.end());  // deterministic order

  std::unordered_map<std::string, std::unordered_set<std::string>> reverse;
  for (const auto& [from, toset] : deps) {
    for (const auto& to : toset) reverse[to].insert(from);
  }

  while (!ready.empty()) {
    // Pop smallest name for reproducibility.
    std::sort(ready.begin(), ready.end());
    std::string n = std::move(ready.front());
    ready.erase(ready.begin());
    order.push_back(n);
    auto it = reverse.find(n);
    if (it == reverse.end()) continue;
    for (const auto& dep : it->second) {
      auto& d = indeg[dep];
      if (d > 0) {
        --d;
        if (d == 0) ready.push_back(dep);
      }
    }
  }

  // Anything left with indeg > 0 is either on a cycle or downstream of
  // one.  Downstream-of-cycle nodes may have zero outgoing edges, in
  // which case they are absent from `deps' -- use find() rather than
  // at() to avoid throwing.
  for (const auto& [name, d] : indeg) {
    if (d > 0) {
      auto it = deps.find(name);
      if (it == deps.end()) continue;
      for (const auto& dep : it->second) {
        cycle_edges.emplace_back(name, dep);
      }
    }
  }
  return TopoResult{.order = std::move(order),
                    .cycle_edges = std::move(cycle_edges)};
}

const ParsedUnit* UnitGraph::lookup(std::string_view name) const {
  auto it = units_.find(to_lower(name));
  if (it == units_.end()) return nullptr;
  return &it->second;
}

}  // namespace tp2cc
