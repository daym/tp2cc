// Tests for UnitGraph (discovery via uses-walk + topological ordering).
//
// Each test writes small synthetic units to a temporary directory and drives
// UnitGraph from a program entry, so these tests don't depend on the real
// fpc sources. One final smoke test does run against rpm/compiler/ to
// confirm the real graph is acyclic and complete.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "diag.h"
#include "test_util.h"
#include "typereg.h"
#include "units.h"

using namespace tp2cc;
using namespace tp2cc_test;

namespace fs = std::filesystem;

namespace {

fs::path make_tmpdir(const char* tag) {
  fs::path base = fs::temp_directory_path() / "tp2cc-test";
  fs::create_directories(base);
  char nm[64];
  std::snprintf(nm, sizeof(nm), "%s-%d", tag, (int)::getpid());
  fs::path d = base / nm;
  if (fs::exists(d)) fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

void write_file(const fs::path& p, std::string_view text) {
  std::ofstream f(p);
  f << text;
}

struct CurrentPathGuard {
  fs::path old;

  explicit CurrentPathGuard(const fs::path& next) : old(fs::current_path()) {
    fs::current_path(next);
  }

  ~CurrentPathGuard() { fs::current_path(old); }
};

void write_unit(const fs::path& dir, const std::string& name,
                const std::string& uses) {
  std::string body = "unit " + name + ";\n";
  body += "interface\n";
  if (!uses.empty()) body += "uses " + uses + ";\n";
  body += "implementation\nend.\n";
  write_file(dir / (name + ".pas"), body);
}

fs::path write_program(const fs::path& dir, const std::string& name,
                       const std::string& uses) {
  std::string body = "program " + name + ";\n";
  if (!uses.empty()) body += "uses " + uses + ";\n";
  body += "begin\nend.\n";
  fs::path p = dir / (name + ".pas");
  write_file(p, body);
  return p;
}

std::vector<std::string> order_of(UnitGraph& g) {
  auto tr = g.topo_sort();
  return tr.order;
}

int index_of(const std::vector<std::string>& v, const std::string& n) {
  for (int i = 0; i < (int)v.size(); ++i) if (v[i] == n) return i;
  return -1;
}

void test_single_unit() {
  auto d = make_tmpdir("single");
  write_unit(d, "foo", "");
  auto prog = write_program(d, "main", "foo");
  UnitGraph g;
  g.add_search_root(d);
  CHECK_EQ(g.discover_from_entry(prog), 0);
  // main + foo
  CHECK_EQ(g.units().size(), size_t{2});
  auto order = order_of(g);
  CHECK(index_of(order, "foo") < index_of(order, "main"));
  fs::remove_all(d);
}

void test_linear_chain() {
  auto d = make_tmpdir("chain");
  // main -> a -> b -> c
  write_unit(d, "a", "b");
  write_unit(d, "b", "c");
  write_unit(d, "c", "");
  auto prog = write_program(d, "main", "a");
  UnitGraph g;
  g.add_search_root(d);
  CHECK_EQ(g.discover_from_entry(prog), 0);
  auto order = order_of(g);
  CHECK_EQ(order.size(), size_t{4});
  CHECK(index_of(order, "c") < index_of(order, "b"));
  CHECK(index_of(order, "b") < index_of(order, "a"));
  CHECK(index_of(order, "a") < index_of(order, "main"));
  fs::remove_all(d);
}

void test_diamond() {
  auto d = make_tmpdir("diamond");
  // top -> {b, c} -> base
  write_unit(d, "base", "");
  write_unit(d, "b", "base");
  write_unit(d, "c", "base");
  write_unit(d, "top", "b, c");
  auto prog = write_program(d, "main", "top");
  UnitGraph g;
  g.add_search_root(d);
  CHECK_EQ(g.discover_from_entry(prog), 0);
  auto order = order_of(g);
  CHECK_EQ(order.size(), size_t{5});
  int i_base = index_of(order, "base");
  int i_b    = index_of(order, "b");
  int i_c    = index_of(order, "c");
  int i_top  = index_of(order, "top");
  CHECK(i_base < i_b);
  CHECK(i_base < i_c);
  CHECK(i_b < i_top);
  CHECK(i_c < i_top);
  fs::remove_all(d);
}

void test_case_insensitive() {
  auto d = make_tmpdir("case");
  write_unit(d, "alpha", "");
  write_unit(d, "beta",  "ALPHA");   // mixed-case reference
  auto prog = write_program(d, "main", "beta");
  UnitGraph g;
  g.add_search_root(d);
  CHECK_EQ(g.discover_from_entry(prog), 0);
  auto order = order_of(g);
  CHECK_EQ(order.size(), size_t{3});
  CHECK(index_of(order, "alpha") < index_of(order, "beta"));
  fs::remove_all(d);
}

void test_runtime_backed_uses_do_not_require_source() {
  // Runtime-backed units have declared exports but no source file in the
  // translated graph. They are allowed by name; arbitrary missing units are not.
  auto d = make_tmpdir("runtime_units");
  int before = error_count();
  write_unit(d, "solo", "baseunix, dos, linux, math, strings, sysutils, unix");
  auto prog = write_program(d, "main", "solo");
  UnitGraph g;
  g.add_search_root(d);
  CHECK_EQ(g.discover_from_entry(prog), 0);
  CHECK_EQ(error_count(), before);
  auto tr = g.topo_sort();
  CHECK_EQ(tr.order.size(), size_t{2});
  CHECK(tr.cycle_edges.empty());
  fs::remove_all(d);
}

void test_missing_uses_reports_error() {
  auto d = make_tmpdir("missing_unit");
  int before = error_count();
  write_unit(d, "solo", "not_a_runtime_unit");
  auto prog = write_program(d, "main", "solo");
  UnitGraph g;
  g.add_search_root(d);
  CHECK(g.discover_from_entry(prog) > 0);
  CHECK(error_count() > before);
  fs::remove_all(d);
}

void test_cycle_detected() {
  auto d = make_tmpdir("cycle");
  // Cycle inside the uses-reachable graph: a <-> b.
  write_unit(d, "a", "b");
  write_unit(d, "b", "a");
  auto prog = write_program(d, "main", "a");
  UnitGraph g;
  g.add_search_root(d);
  (void)g.discover_from_entry(prog);
  auto tr = g.topo_sort();
  CHECK(!tr.cycle_edges.empty());
  fs::remove_all(d);
}

void test_discover_from_entry_only_reachable_units() {
  auto d = make_tmpdir("entry");
  fs::create_directories(d / "sub");
  auto main = write_program(d, "main", "alpha");
  write_unit(d, "alpha", "beta");
  write_unit(d / "sub", "beta", "");
  write_unit(d, "unused", "");

  UnitGraph g;
  // Search roots are non-recursive; subdirs must be listed explicitly.
  g.add_search_root(d);
  g.add_search_root(d / "sub");
  CHECK_EQ(g.discover_from_entry(main), 0);
  CHECK_EQ(g.units().size(), size_t{3});
  CHECK(g.lookup("main") != nullptr);
  CHECK(g.lookup("alpha") != nullptr);
  CHECK(g.lookup("beta") != nullptr);
  CHECK(g.lookup("unused") == nullptr);
  fs::remove_all(d);
}

void test_current_directory_is_implicit_unit_search_root() {
  auto d = make_tmpdir("implicit-cwd");
  fs::path cwd = d / "cwd";
  fs::path main_dir = d / "main";
  fs::create_directories(cwd);
  fs::create_directories(main_dir);
  write_unit(cwd, "foo", "");
  auto prog = write_program(main_dir, "main", "foo");

  {
    CurrentPathGuard guard(cwd);
    UnitGraph g;
    CHECK_EQ(g.discover_from_entry(prog), 0);
    const ParsedUnit* foo = g.lookup("foo");
    CHECK(foo != nullptr);
    if (foo) CHECK(fs::equivalent(foo->path, cwd / "foo.pas"));
  }

  fs::remove_all(d);
}

void test_entry_directory_is_implicit_unit_search_root() {
  auto d = make_tmpdir("implicit-entry");
  write_unit(d, "foo", "");
  auto prog = write_program(d, "main", "foo");
  UnitGraph g;
  CHECK_EQ(g.discover_from_entry(prog), 0);
  const ParsedUnit* foo = g.lookup("foo");
  CHECK(foo != nullptr);
  if (foo) CHECK(fs::equivalent(foo->path, d / "foo.pas"));
  fs::remove_all(d);
}

void test_current_directory_precedes_entry_directory_and_unit_paths() {
  auto d = make_tmpdir("implicit-order");
  fs::path cwd = d / "cwd";
  fs::path main_dir = d / "main";
  fs::path fu_dir = d / "fu";
  fs::create_directories(cwd);
  fs::create_directories(main_dir);
  fs::create_directories(fu_dir);
  write_unit(cwd, "foo", "");
  write_unit(main_dir, "foo", "");
  write_unit(fu_dir, "foo", "");
  auto prog = write_program(main_dir, "main", "foo");

  {
    CurrentPathGuard guard(cwd);
    UnitGraph g;
    g.add_search_root(fu_dir);
    CHECK_EQ(g.discover_from_entry(prog), 0);
    const ParsedUnit* foo = g.lookup("foo");
    CHECK(foo != nullptr);
    if (foo) CHECK(fs::equivalent(foo->path, cwd / "foo.pas"));
  }

  fs::remove_all(d);
}

void test_parser_driven_type_binding_across_units_and_forwards() {
  auto d = make_tmpdir("semantic-types");
  write_file(
      d / "types.pas",
      "unit types;\n"
      "interface\n"
      "type\n"
      "  pnode = ^tnode;\n"
      "  tnode = record next : pnode; end;\n"
      "  talias = tnode;\n"
      "implementation\n"
      "end.\n");
  auto program = write_program(d, "main", "types");
  write_file(
      program,
      "program main;\n"
      "uses types;\n"
      "var value : talias;\n"
      "begin\n"
      "end.\n");

  UnitGraph graph;
  graph.add_search_root(d);
  CHECK_EQ(graph.discover_from_entry(program), 0);

  const ParsedUnit* types = graph.lookup("types");
  const ParsedUnit* main = graph.lookup("main");
  CHECK(types && types->ast);
  CHECK(main && main->ast);
  if (types && types->ast && main && main->ast) {
    const auto* node_decl = static_cast<const ast::TypeDecl*>(
        types->ast->interface_decls[1].get());
    const auto* alias_decl = static_cast<const ast::TypeDecl*>(
        types->ast->interface_decls[2].get());
    const auto* value_decl = static_cast<const ast::VarDecl*>(
        main->ast->impl_decls[0].get());
    CHECK(node_decl->symbol && node_decl->type->descriptor);
    CHECK_EQ(alias_decl->type->descriptor, node_decl->type->descriptor);
    CHECK_EQ(value_decl->type->descriptor, node_decl->type->descriptor);

    const auto* pointer_decl = static_cast<const ast::TypeDecl*>(
        types->ast->interface_decls[0].get());
    const auto* pointer_type =
        static_cast<const ast::TyPointer*>(pointer_decl->type.get());
    CHECK_EQ(pointer_type->target->descriptor, node_decl->type->descriptor);
  }
  fs::remove_all(d);
}

void test_parser_driven_imported_class_parent_and_alias() {
  auto d = make_tmpdir("semantic-imported-class-parent");
  write_file(
      d / "base.pas",
      "unit base;\n"
      "interface\n"
      "type\n"
      "  tbase = class\n"
      "    value : longint;\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  write_file(
      d / "middle.pas",
      "unit middle;\n"
      "interface\n"
      "uses base;\n"
      "type\n"
      "  tderived = class(tbase)\n"
      "  end;\n"
      "implementation\n"
      "end.\n");
  write_file(
      d / "aliases.pas",
      "unit aliases;\n"
      "interface\n"
      "uses middle;\n"
      "type\n"
      "  talias = tderived;\n"
      "implementation\n"
      "end.\n");
  auto program = write_program(d, "main", "aliases");
  write_file(
      program,
      "program main;\n"
      "uses aliases;\n"
      "var item : talias;\n"
      "begin\n"
      "  item.value := 1;\n"
      "end.\n");

  UnitGraph graph;
  graph.add_search_root(d);
  CHECK_EQ(graph.discover_from_entry(program), 0);

  const TypeRegistry& registry = graph.type_registry();
  const TypeSymbol* base = registry.lookup_type_symbol_exact(
      pascal_key("base"), pascal_key("tbase"));
  const TypeSymbol* derived = registry.lookup_type_symbol_exact(
      pascal_key("middle"), pascal_key("tderived"));
  const TypeSymbol* alias = registry.lookup_type_symbol_exact(
      pascal_key("aliases"), pascal_key("talias"));
  CHECK(base && derived && alias);
  if (base && derived && alias) {
    CHECK(derived->class_info());
    if (derived->class_info()) {
      CHECK_EQ(derived->class_info()->parent_symbol, base);
    }
    CHECK_EQ(alias->descriptor, derived->descriptor);
  }
  fs::remove_all(d);
}

void test_parser_driven_type_binding_rejects_ordinary_later_type() {
  auto d = make_tmpdir("semantic-later-type");
  auto program = d / "main.pas";
  write_file(
      program,
      "program main;\n"
      "type\n"
      "  tfirst = tsecond;\n"
      "  tsecond = record end;\n"
      "begin\n"
      "end.\n");

  const int errors_before = error_count();
  UnitGraph graph;
  graph.add_search_root(d);
  CHECK(graph.discover_from_entry(program) > 0);
  CHECK(error_count() > errors_before);
  const ParsedUnit* main = graph.lookup("main");
  CHECK(main != nullptr);
  if (main) CHECK(!main->ok);
  fs::remove_all(d);
}

void test_implementation_binding_error_keeps_unit_failed() {
  auto d = make_tmpdir("implementation-binding-error");
  auto program = d / "main.pas";
  write_file(
      program,
      "unit main;\n"
      "interface\n"
      "implementation\n"
      "type\n"
      "  tbad = tmissing;\n"
      "end.\n");

  const int errors_before = error_count();
  UnitGraph graph;
  graph.add_search_root(d);
  CHECK(graph.discover_from_entry(program) > 0);
  CHECK(error_count() > errors_before);
  const ParsedUnit* main = graph.lookup("main");
  CHECK(main != nullptr);
  if (main) CHECK(!main->ok);
  fs::remove_all(d);
}

void test_nested_unresolved_types_keep_unit_failed() {
  auto d = make_tmpdir("nested-unresolved-types");
  auto program = d / "main.pas";
  write_file(
      program,
      "unit main;\n"
      "interface\n"
      "type\n"
      "  trecord = record field : tmissingfield; end;\n"
      "  tarray = array[0..1] of tmissingelement;\n"
      "procedure run(value : tmissingparam);\n"
      "implementation\n"
      "procedure run(value : tmissingparam);\n"
      "begin\n"
      "end;\n"
      "end.\n");

  const int errors_before = error_count();
  UnitGraph graph;
  graph.add_search_root(d);
  CHECK(graph.discover_from_entry(program) > 0);
  CHECK(error_count() >= errors_before + 3);
  const ParsedUnit* main = graph.lookup("main");
  CHECK(main != nullptr);
  if (main) CHECK(!main->ok);
  fs::remove_all(d);
}

void test_failed_interface_dependency_keeps_importer_failed() {
  auto d = make_tmpdir("failed-interface-dependency");
  write_file(
      d / "broken.pas",
      "unit broken;\n"
      "interface\n"
      "type tbad = tmissing;\n"
      "implementation\n"
      "end.\n");
  auto program = write_program(d, "main", "broken");

  const int errors_before = error_count();
  UnitGraph graph;
  graph.add_search_root(d);
  CHECK(graph.discover_from_entry(program) > 0);
  CHECK(error_count() > errors_before);
  const ParsedUnit* broken = graph.lookup("broken");
  const ParsedUnit* main = graph.lookup("main");
  CHECK(broken != nullptr);
  CHECK(main != nullptr);
  if (broken) CHECK(!broken->ok);
  if (main) CHECK(!main->ok);
  fs::remove_all(d);
}

void test_failed_dependency_implementation_propagates_after_parsing() {
  auto d = make_tmpdir("failed-dependency-implementation");
  write_file(
      d / "broken.pas",
      "unit broken;\n"
      "interface\n"
      "procedure run;\n"
      "implementation\n"
      "procedure run;\n"
      "type tbad = tmissing;\n"
      "begin\n"
      "end;\n"
      "end.\n");
  write_unit(d, "good", "");
  auto program = d / "main.pas";
  write_file(
      program,
      "program main;\n"
      "uses broken, good;\n"
      "begin\n"
      "end.\n");

  const int errors_before = error_count();
  UnitGraph graph;
  graph.add_search_root(d);
  CHECK(graph.discover_from_entry(program) > 0);
  CHECK(error_count() > errors_before);
  const ParsedUnit* broken = graph.lookup("broken");
  const ParsedUnit* good = graph.lookup("good");
  const ParsedUnit* main = graph.lookup("main");
  CHECK(broken != nullptr);
  CHECK(good != nullptr);
  CHECK(main != nullptr);
  if (broken) CHECK(!broken->ok);
  if (good) CHECK(good->ok);
  if (main) CHECK(!main->ok);
  fs::remove_all(d);
}

void test_parser_driven_type_binding_allows_class_self_reference() {
  auto d = make_tmpdir("semantic-class-self");
  auto program = d / "main.pas";
  write_file(
      program,
      "program main;\n"
      "type\n"
      "  tnode = class\n"
      "    next : tnode;\n"
      "  end;\n"
      "begin\n"
      "end.\n");

  UnitGraph graph;
  graph.add_search_root(d);
  CHECK_EQ(graph.discover_from_entry(program), 0);
  const ParsedUnit* main = graph.lookup("main");
  CHECK(main && main->ast);
  if (main && main->ast) {
    const auto* node_decl = static_cast<const ast::TypeDecl*>(
        main->ast->impl_decls[0].get());
    const auto* node_type =
        static_cast<const ast::TyObject*>(node_decl->type.get());
    const auto& field = node_type->members[0];
    CHECK(node_decl->symbol && node_decl->type->descriptor);
    CHECK_EQ(field.field_type->descriptor, node_decl->type->descriptor);
  }
  fs::remove_all(d);
}

void test_parser_driven_type_binding_completes_explicit_class_forward() {
  auto d = make_tmpdir("semantic-class-forward");
  auto program = d / "main.pas";
  write_file(
      program,
      "program main;\n"
      "type\n"
      "  tnode = class;\n"
      "type\n"
      "  tnode = class\n"
      "    next : tnode;\n"
      "  end;\n"
      "var value : tnode;\n"
      "begin\n"
      "end.\n");

  UnitGraph graph;
  graph.add_search_root(d);
  CHECK_EQ(graph.discover_from_entry(program), 0);
  const ParsedUnit* main = graph.lookup("main");
  CHECK(main && main->ast);
  if (main && main->ast) {
    const auto* forward_decl = static_cast<const ast::TypeDecl*>(
        main->ast->impl_decls[0].get());
    const auto* complete_decl = static_cast<const ast::TypeDecl*>(
        main->ast->impl_decls[1].get());
    const auto* value_decl = static_cast<const ast::VarDecl*>(
        main->ast->impl_decls[2].get());
    CHECK(forward_decl->symbol && complete_decl->symbol);
    CHECK_EQ(forward_decl->symbol, complete_decl->symbol);
    CHECK_EQ(forward_decl->type->descriptor,
             complete_decl->type->descriptor);
    CHECK_EQ(value_decl->type->descriptor,
             complete_decl->type->descriptor);
  }
  fs::remove_all(d);
}

void test_parser_driven_local_types_remain_in_procedure_scope() {
  auto d = make_tmpdir("semantic-local-type");
  auto program = d / "main.pas";
  write_file(
      program,
      "program main;\n"
      "procedure run;\n"
      "type\n"
      "  tlocal = record value : longint; end;\n"
      "var item : tlocal;\n"
      "begin\n"
      "end;\n"
      "begin\n"
      "end.\n");

  UnitGraph graph;
  graph.add_search_root(d);
  CHECK_EQ(graph.discover_from_entry(program), 0);
  const ParsedUnit* main = graph.lookup("main");
  CHECK(main && main->ast);
  if (main && main->ast) {
    const auto* proc = static_cast<const ast::ProcDecl*>(
        main->ast->impl_decls[0].get());
    const auto* local_type =
        static_cast<const ast::TypeDecl*>(proc->locals[0].get());
    const auto* local_var =
        static_cast<const ast::VarDecl*>(proc->locals[1].get());
    CHECK(local_type->symbol && local_type->type->descriptor);
    CHECK_EQ(local_var->type->descriptor, local_type->type->descriptor);
    CHECK_EQ(graph.type_registry().lookup_type_symbol_exact(
                 pascal_key("main"), pascal_key("tlocal")),
             nullptr);
  }
  fs::remove_all(d);
}

void test_parser_driven_local_pointer_forward_stays_in_type_section() {
  auto d = make_tmpdir("semantic-local-pointer-forward");
  auto program = d / "main.pas";
  write_file(
      program,
      "program main;\n"
      "procedure run;\n"
      "type\n"
      "  pnode = ^tnode;\n"
      "  tnode = record next : pnode; end;\n"
      "var item : tnode;\n"
      "begin\n"
      "end;\n"
      "begin\n"
      "end.\n");

  UnitGraph graph;
  graph.add_search_root(d);
  CHECK_EQ(graph.discover_from_entry(program), 0);
  const ParsedUnit* main = graph.lookup("main");
  CHECK(main && main->ast);
  if (main && main->ast) {
    const auto* proc = static_cast<const ast::ProcDecl*>(
        main->ast->impl_decls[0].get());
    const auto* pointer_decl =
        static_cast<const ast::TypeDecl*>(proc->locals[0].get());
    const auto* record_decl =
        static_cast<const ast::TypeDecl*>(proc->locals[1].get());
    const auto* pointer_type =
        static_cast<const ast::TyPointer*>(pointer_decl->type.get());
    CHECK_EQ(pointer_decl->type_section_id, record_decl->type_section_id);
    CHECK_EQ(pointer_type->target->descriptor,
             record_decl->type->descriptor);
  }
  fs::remove_all(d);
}

void test_parser_driven_local_pointer_forward_rejects_later_section() {
  auto d = make_tmpdir("semantic-local-pointer-later-section");
  auto program = d / "main.pas";
  write_file(
      program,
      "program main;\n"
      "procedure run;\n"
      "type\n"
      "  pnode = ^tnode;\n"
      "type\n"
      "  tnode = record next : pnode; end;\n"
      "begin\n"
      "end;\n"
      "begin\n"
      "end.\n");

  const int errors_before = error_count();
  UnitGraph graph;
  graph.add_search_root(d);
  CHECK(graph.discover_from_entry(program) > 0);
  CHECK(error_count() > errors_before);
  fs::remove_all(d);
}

void test_parser_driven_nested_pointer_forward_stays_in_type_section() {
  auto d = make_tmpdir("semantic-nested-pointer-forward");
  auto program = d / "main.pas";
  write_file(
      program,
      "program main;\n"
      "type\n"
      "  towner = record\n"
      "  type\n"
      "    pnode = ^tnode;\n"
      "    tnode = record next : pnode; end;\n"
      "  end;\n"
      "begin\n"
      "end.\n");

  UnitGraph graph;
  graph.add_search_root(d);
  CHECK_EQ(graph.discover_from_entry(program), 0);
  const ParsedUnit* main = graph.lookup("main");
  CHECK(main && main->ast);
  if (main && main->ast) {
    const auto* owner_decl = static_cast<const ast::TypeDecl*>(
        main->ast->impl_decls[0].get());
    const auto* owner =
        static_cast<const ast::TyRecord*>(owner_decl->type.get());
    const auto* pointer_decl = owner->nested_types[0].get();
    const auto* record_decl = owner->nested_types[1].get();
    const auto* pointer_type =
        static_cast<const ast::TyPointer*>(pointer_decl->type.get());
    CHECK_EQ(pointer_decl->type_section_id, record_decl->type_section_id);
    CHECK_EQ(pointer_type->target->descriptor,
             record_decl->type->descriptor);
  }
  fs::remove_all(d);
}

void test_parser_driven_nested_pointer_forward_rejects_later_section() {
  auto d = make_tmpdir("semantic-nested-pointer-later-section");
  auto program = d / "main.pas";
  write_file(
      program,
      "program main;\n"
      "type\n"
      "  towner = record\n"
      "  type\n"
      "    pnode = ^tnode;\n"
      "  type\n"
      "    tnode = record next : pnode; end;\n"
      "  end;\n"
      "begin\n"
      "end.\n");

  const int errors_before = error_count();
  UnitGraph graph;
  graph.add_search_root(d);
  CHECK(graph.discover_from_entry(program) > 0);
  CHECK(error_count() > errors_before);
  fs::remove_all(d);
}

void test_real_fpc_compiler_acyclic() {
  // Optional rpm/compiler fixture: walking pp.pas with the bootstrap defines
  // should not find a unit cycle.
  fs::path src_root = fs::path(__FILE__).parent_path().parent_path().parent_path();
  fs::path compiler = src_root / "rpm" / "compiler";
  fs::path entry = compiler / "pp.pas";
  if (!fs::exists(entry)) {
    // Skip silently if not run from the expected tree layout.
    return;
  }
  UnitGraph g;
  g.add_search_root(compiler);
  g.define("FPC");
  g.define("I386");
  g.define("LINUX");
  g.define("UNIX");
  g.define("NOTARGETWIN32");
  (void)g.discover_from_entry(entry);
  auto tr = g.topo_sort();
  CHECK(tr.cycle_edges.empty());
  CHECK(g.units().size() > 50);
  CHECK_EQ(tr.order.size(), g.units().size());
}

}  // namespace

int main() {
  RUN_TEST(test_single_unit);
  RUN_TEST(test_linear_chain);
  RUN_TEST(test_diamond);
  RUN_TEST(test_case_insensitive);
  RUN_TEST(test_runtime_backed_uses_do_not_require_source);
  RUN_TEST(test_missing_uses_reports_error);
  RUN_TEST(test_cycle_detected);
  RUN_TEST(test_discover_from_entry_only_reachable_units);
  RUN_TEST(test_current_directory_is_implicit_unit_search_root);
  RUN_TEST(test_entry_directory_is_implicit_unit_search_root);
  RUN_TEST(test_current_directory_precedes_entry_directory_and_unit_paths);
  RUN_TEST(test_parser_driven_type_binding_across_units_and_forwards);
  RUN_TEST(test_parser_driven_imported_class_parent_and_alias);
  RUN_TEST(test_parser_driven_type_binding_rejects_ordinary_later_type);
  RUN_TEST(test_implementation_binding_error_keeps_unit_failed);
  RUN_TEST(test_nested_unresolved_types_keep_unit_failed);
  RUN_TEST(test_failed_interface_dependency_keeps_importer_failed);
  RUN_TEST(test_failed_dependency_implementation_propagates_after_parsing);
  RUN_TEST(test_parser_driven_type_binding_allows_class_self_reference);
  RUN_TEST(test_parser_driven_type_binding_completes_explicit_class_forward);
  RUN_TEST(test_parser_driven_local_types_remain_in_procedure_scope);
  RUN_TEST(test_parser_driven_local_pointer_forward_stays_in_type_section);
  RUN_TEST(
      test_parser_driven_local_pointer_forward_rejects_later_section);
  RUN_TEST(test_parser_driven_nested_pointer_forward_stays_in_type_section);
  RUN_TEST(
      test_parser_driven_nested_pointer_forward_rejects_later_section);
  RUN_TEST(test_real_fpc_compiler_acyclic);

  int n = tp2cc_test::failures();
  std::printf("%s: %d failure%s\n", (n == 0 ? "PASS" : "FAIL"), n,
              (n == 1 ? "" : "s"));
  return n == 0 ? 0 : 1;
}
