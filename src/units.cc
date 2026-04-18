#include "units.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "diag.h"
#include "lexer.h"
#include "parser.h"

namespace fs = std::filesystem;

namespace p2cc {

UnitGraph::UnitGraph() = default;

void UnitGraph::add_search_root(fs::path p) { roots_.push_back(std::move(p)); }

void UnitGraph::define(std::string name) { defines_.push_back(std::move(name)); }

void UnitGraph::skip_path_containing(std::string needle) {
  skip_needles_.push_back(std::move(needle));
}

bool UnitGraph::skipped(const std::string& path) const {
  for (const auto& n : skip_needles_) {
    if (path.find(n) != std::string::npos) return true;
  }
  return false;
}

std::string UnitGraph::to_lower(std::string_view s) {
  std::string r(s);
  for (auto& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return r;
}

int UnitGraph::discover() {
  int errors = 0;
  for (const auto& root : roots_) {
    if (!fs::exists(root)) continue;
    for (auto& e : fs::recursive_directory_iterator(root)) {
      if (!e.is_regular_file()) continue;
      auto ext = e.path().extension();
      if (ext != ".pas" && ext != ".pp") continue;
      auto s = e.path().string();
      if (skipped(s)) continue;

      auto sf = SourceFile::load(e.path());
      if (!sf) { ++errors; continue; }

      int errs_before = error_count();
      Lexer lex(std::move(sf));
      for (const auto& d : defines_) lex.define(d);
      Parser parser(lex);
      auto node = parser.parse();
      int errs = error_count() - errs_before;

      ParsedUnit pu;
      pu.path = e.path();
      pu.ok = (errs == 0 && node != nullptr);
      if (node) pu.name = to_lower(node->name);
      pu.ast = std::move(node);

      if (!pu.ok) { errors += errs > 0 ? errs : 1; }

      if (pu.name.empty()) {
        // A program, or a failed parse. Use the filename stem as a fallback
        // key so programs don't collide with each other. Programs are not
        // referenced via `uses`, so this key won't match any dependency.
        pu.name = std::string("__prog_") + e.path().stem().string();
      }

      auto it = units_.find(pu.name);
      if (it != units_.end()) {
        // Duplicate unit name -- could happen if someone has two copies of
        // a unit on the search path. Report and keep the first.
        Location loc;
        if (pu.ast) loc = pu.ast->loc;
        report_warning(loc, "duplicate unit '" + pu.name + "' at " +
                                 e.path().string() + " (first seen at " +
                                 it->second.path.string() + ")");
        continue;
      }
      units_.emplace(pu.name, std::move(pu));
    }
  }
  return errors;
}

UnitGraph::TopoResult UnitGraph::topo_sort() const {
  TopoResult out;

  // Build adjacency: for each unit, its (lowercased) uses entries. Edges
  // are reversed for Kahn's algorithm.
  std::unordered_map<std::string, std::unordered_set<std::string>> deps;
  std::unordered_map<std::string, int> indeg;

  for (const auto& [name, pu] : units_) {
    indeg[name] = 0;
  }
  auto add_dep = [&](const std::string& from, const std::string& to) {
    if (!units_.count(to)) {
      // External unit (e.g. "dos", "linux", "strings" from RTL). Not in our
      // graph; we'll link it via the hand-written runtime. Skip.
      return;
    }
    if (deps[from].insert(to).second) {
      indeg[from] += 1;
    }
  };

  // Pascal semantics: only `interface uses` lists form the compile-order
  // graph (and must be acyclic). `implementation uses` may form cycles
  // because implementation bodies are free to reference each other after
  // all interfaces have been established. We therefore build edges only
  // from interface_uses for topo sort; the emitter will still need
  // impl_uses in the generated `.cc` includes.
  for (const auto& [name, pu] : units_) {
    if (!pu.ast) continue;
    for (const auto& u : pu.ast->interface_uses) add_dep(name, to_lower(u));
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
    out.order.push_back(n);
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

  // Anything left with indeg > 0 is on a cycle.
  for (const auto& [name, d] : indeg) {
    if (d > 0) {
      for (const auto& dep : deps.at(name)) {
        out.cycle_edges.emplace_back(name, dep);
      }
    }
  }
  return out;
}

const ParsedUnit* UnitGraph::lookup(std::string_view name) const {
  auto it = units_.find(to_lower(name));
  if (it == units_.end()) return nullptr;
  return &it->second;
}

}  // namespace p2cc
