#include "units.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "runtime_units.h"
#include "typereg.h"

namespace fs = std::filesystem;

namespace tp2cc {

struct UnitGraph::ParseState {
  enum class Phase {
    ParsingInterface,
    InterfaceReady,
    Complete,
    Failed,
  };

  struct Session;

  struct Actions final : ParserSemanticActions {
    UnitGraph& graph;
    Session& session;

    Actions(UnitGraph& graph_in, Session& session_in)
        : graph(graph_in), session(session_in) {}

    void begin_compilation_unit(std::string_view name,
                                bool is_program) override;
    void import_units(const std::vector<std::string>& units,
                      bool in_interface) override;
    void parsed_type_section(
        const std::vector<ast::DeclPtr>& declarations,
        bool in_interface) override;
    void parsed_declaration(const ast::DeclPtr& declaration,
                            bool in_interface) override;
    void finish_compilation_unit(const ast::UnitNode& unit) override;
  };

  struct Session {
    std::filesystem::path path;
    std::string name;
    bool is_program = false;
    // Interface parsing, deferred implementation parsing, and semantic
    // callbacks all contribute to one compilation result. Once any phase
    // fails, a later non-null AST must not make the unit successful again.
    bool had_error = false;
    Phase phase = Phase::ParsingInterface;
    std::unique_ptr<Lexer> lexer;
    std::unique_ptr<Actions> actions;
    std::unique_ptr<Parser> parser;
    std::shared_ptr<ast::UnitNode> ast;
  };

  TypeRegistry registry;
  std::unordered_map<std::string, std::unique_ptr<Session>> sessions_by_path;
  std::unordered_map<std::string, Session*> sessions_by_name;
};

void UnitGraph::ParseState::Actions::begin_compilation_unit(
    std::string_view name, bool is_program) {
  session.name = UnitGraph::to_lower(name);
  session.is_program = is_program;
  auto existing = graph.parse_state_->sessions_by_name.find(session.name);
  if (existing != graph.parse_state_->sessions_by_name.end() &&
      existing->second != &session) {
    report_warning({}, "duplicate unit '" + session.name + "' at " +
                           session.path.string());
  } else {
    graph.parse_state_->sessions_by_name[session.name] = &session;
  }
  graph.parse_state_->registry.begin_parsed_unit(session.name);
  graph.units_.try_emplace(
      session.name,
      ParsedUnit{.name = session.name,
                 .path = session.path,
                 .ast = nullptr,
                 .ok = false});
}

void UnitGraph::ParseState::Actions::import_units(
    const std::vector<std::string>& imports, bool in_interface) {
  for (const std::string& imported : imports) {
    const std::string unit = UnitGraph::to_lower(imported);
    if (has_runtime_unit_model(unit)) continue;
    auto existing = graph.parse_state_->sessions_by_name.find(unit);
    if (existing != graph.parse_state_->sessions_by_name.end()) {
      if (existing->second->phase == Phase::ParsingInterface) {
        report_error({}, "interface uses cycle involving `" + session.name +
                             "` and `" + unit + "`");
        session.had_error = true;
      } else if (existing->second->had_error ||
                 existing->second->phase == Phase::Failed) {
        session.had_error = true;
      }
      continue;
    }
    const fs::path dependency = graph.find_unit_path(unit);
    if (dependency.empty()) {
      report_error({}, "unresolved unit `" + unit + "` used by `" +
                           session.name + "`");
      session.had_error = true;
      continue;
    }
    if (graph.parse_recursive(dependency) != 0) {
      session.had_error = true;
    }
  }
  graph.parse_state_->registry.set_parsed_unit_imports(
      session.name, imports, in_interface);
}

void UnitGraph::ParseState::Actions::parsed_type_section(
    const std::vector<ast::DeclPtr>& declarations, bool in_interface) {
  graph.parse_state_->registry.bind_parsed_declarations(
      session.name, declarations, in_interface);
}

void UnitGraph::ParseState::Actions::parsed_declaration(
    const ast::DeclPtr& declaration, bool in_interface) {
  graph.parse_state_->registry.bind_parsed_declarations(
      session.name, std::vector<ast::DeclPtr>{declaration}, in_interface);
}

void UnitGraph::ParseState::Actions::finish_compilation_unit(
    const ast::UnitNode& unit) {
  graph.parse_state_->registry.bind_parsed_unit_bodies(unit);
}

UnitGraph::UnitGraph() : parse_state_(std::make_unique<ParseState>()) {}
UnitGraph::~UnitGraph() = default;

TypeRegistry& UnitGraph::type_registry() { return parse_state_->registry; }
const TypeRegistry& UnitGraph::type_registry() const {
  return parse_state_->registry;
}

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

int UnitGraph::unit_paths_to_discover(
    const ast::UnitNode& owner, const std::vector<std::string>& uses,
    std::vector<fs::path>* paths) {
  int errors = 0;
  for (const auto& dep : uses) {
    std::string dep_name = to_lower(dep);
    if (units_.count(dep_name)) continue;
    fs::path dep_path = find_unit_path(dep_name);
    if (dep_path.empty()) {
      if (has_runtime_unit_model(dep_name)) continue;
      report_error(owner.loc, "unresolved unit `" + dep_name +
                                  "` used by `" + owner.name + "`");
      ++errors;
      continue;
    }
    paths->push_back(std::move(dep_path));
  }
  return errors;
}

int UnitGraph::parse_recursive(const fs::path& path) {
  const std::string path_key =
      fs::absolute(path).lexically_normal().string();
  if (auto existing = parse_state_->sessions_by_path.find(path_key);
      existing != parse_state_->sessions_by_path.end()) {
    if (existing->second->phase == ParseState::Phase::ParsingInterface) {
      report_error({}, "interface uses cycle at `" + path.string() + "`");
      return 1;
    }
    return existing->second->had_error ||
                   existing->second->phase == ParseState::Phase::Failed
               ? 1
               : 0;
  }

  auto sf = SourceFile::load(path);
  if (!sf) return 1;

  int errs_before = error_count();
  auto session = std::make_unique<ParseState::Session>();
  session->path = path;
  session->lexer = std::make_unique<Lexer>(std::move(sf), include_paths_);
  for (const auto& d : defines_) session->lexer->define(d);
  session->lexer->set_overflow_check_default(overflow_check_default_);
  session->lexer->set_range_check_default(range_check_default_);
  ParseState::Session* active = session.get();
  session->actions =
      std::make_unique<ParseState::Actions>(*this, *active);
  session->parser =
      std::make_unique<Parser>(*session->lexer, session->actions.get());
  parse_state_->sessions_by_path.emplace(path_key, std::move(session));

  std::shared_ptr<ast::UnitNode> node;
  if (active->parser->starts_unit()) {
    node = active->parser->parse_unit_interface();
    active->phase = node ? ParseState::Phase::InterfaceReady
                         : ParseState::Phase::Failed;
  } else {
    node = active->parser->parse();
    active->phase = node ? ParseState::Phase::Complete
                         : ParseState::Phase::Failed;
  }
  int errs = error_count() - errs_before;
  active->had_error = active->had_error || errs != 0;

  std::string unit_name = node ? to_lower(node->name) : std::string{};
  if (unit_name.empty()) {
    unit_name = std::string("__prog_") + path.stem().string();
  }
  active->name = unit_name;
  active->ast = node;
  const bool parsed_ok =
      !active->had_error && node != nullptr &&
      active->phase == ParseState::Phase::Complete;
  ParsedUnit pu{.name = std::move(unit_name),
                .path = path,
                .ast = node,
                .ok = parsed_ok};

  auto it = units_.find(pu.name);
  if (it != units_.end()) {
    if (!it->second.path.empty() && it->second.path != pu.path) {
      report_warning(pu.ast ? pu.ast->loc : Location{},
                     "duplicate unit '" + pu.name + "' at " +
                         path.string() + " (first seen at " +
                         it->second.path.string() + ")");
    }
    it->second.path = pu.path;
    it->second.ast = pu.ast;
    it->second.ok = pu.ok;
    return errs > 0 ? errs : 0;
  }

  std::string key = pu.name;
  units_.emplace(key, std::move(pu));
  return errs > 0 ? errs : 0;
}

int UnitGraph::parse_pending_implementations() {
  int errors_before = error_count();
  for (;;) {
    ParseState::Session* pending = nullptr;
    for (auto& [_, session] : parse_state_->sessions_by_path) {
      if (session->phase == ParseState::Phase::InterfaceReady) {
        pending = session.get();
        break;
      }
    }
    if (!pending) break;
    const int unit_errors_before = error_count();
    std::shared_ptr<ast::UnitNode> node =
        pending->parser->parse_unit_implementation();
    pending->had_error =
        pending->had_error || error_count() != unit_errors_before;
    pending->ast = node;
    pending->phase = node ? ParseState::Phase::Complete
                          : ParseState::Phase::Failed;
    auto found = units_.find(pending->name);
    if (found != units_.end()) {
      found->second.ast = node;
      found->second.ok = pending->phase == ParseState::Phase::Complete &&
                         node != nullptr && !pending->had_error;
    }
  }
  return error_count() - errors_before;
}

int UnitGraph::discover_from_entry(fs::path entry_path) {
  units_.clear();
  parse_state_ = std::make_unique<ParseState>();
  unit_path_index_.clear();
  unit_path_index_ready_ = false;
  if (!entry_path.is_absolute()) entry_path = fs::absolute(entry_path);
  current_dir_ = fs::current_path();
  entry_dir_ = entry_path.parent_path();
  int errors = parse_recursive(entry_path);
  errors += parse_pending_implementations();

  // Implementations are intentionally parsed after every reachable
  // interface, so an implementation failure can be discovered after its
  // importers were parsed. Propagate dependency failure only after both
  // phases are complete; unrelated units remain usable by emit-all.
  bool changed;
  do {
    changed = false;
    for (auto& [_, unit] : units_) {
      if (!unit.ok || !unit.ast) continue;
      auto dependency_failed = [&](const std::vector<std::string>& uses) {
        for (const std::string& dependency : uses) {
          auto found = units_.find(to_lower(dependency));
          if (found != units_.end() && !found->second.ok) return true;
        }
        return false;
      };
      if (dependency_failed(unit.ast->interface_uses) ||
          dependency_failed(unit.ast->impl_uses)) {
        unit.ok = false;
        changed = true;
      }
    }
  } while (changed);
  return errors;
}

void UnitGraph::add_topo_dependency(
    const std::unordered_map<std::string, ParsedUnit>& units,
    std::unordered_map<std::string, std::unordered_set<std::string>>& deps,
    std::unordered_map<std::string, int>& indeg, const std::string& from,
    std::string_view to) {
  std::string canonical_to = to_lower(to);
  // Topology is only for units parsed from source. Runtime-backed units are
  // registered as ordinary UnitInfos later, but they do not add source files to
  // the translation graph.
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
