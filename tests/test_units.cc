// Tests for UnitGraph (discovery + topological ordering).
//
// We write small synthetic units to a temporary directory and drive
// UnitGraph against them, so these tests don't depend on the real fpc
// sources. One final smoke test does run against rpm/compiler/ to confirm
// the real graph is acyclic and complete.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "test_util.h"
#include "units.h"

using namespace p2cc;
using namespace p2cc_test;

namespace fs = std::filesystem;

namespace {

fs::path make_tmpdir(const char* tag) {
  fs::path base = fs::temp_directory_path() / "p2cc-test";
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

void write_unit(const fs::path& dir, const std::string& name,
                const std::string& uses) {
  std::string body = "unit " + name + ";\n";
  body += "interface\n";
  if (!uses.empty()) body += "uses " + uses + ";\n";
  body += "implementation\nend.\n";
  write_file(dir / (name + ".pas"), body);
}

void write_program(const fs::path& path, const std::string& name,
                   const std::string& uses) {
  std::string body = "program " + name + ";\n";
  if (!uses.empty()) body += "uses " + uses + ";\n";
  body += "begin\nend.\n";
  write_file(path, body);
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
  UnitGraph g;
  g.add_search_root(d);
  CHECK_EQ(g.discover(), 0);
  CHECK_EQ(g.units().size(), size_t{1});
  auto tr = g.topo_sort();
  CHECK_EQ(tr.order.size(), size_t{1});
  CHECK_EQ(tr.order[0], std::string("foo"));
  CHECK(tr.cycle_edges.empty());
  fs::remove_all(d);
}

void test_linear_chain() {
  auto d = make_tmpdir("chain");
  // a uses b uses c uses (nothing)
  write_unit(d, "a", "b");
  write_unit(d, "b", "c");
  write_unit(d, "c", "");
  UnitGraph g;
  g.add_search_root(d);
  CHECK_EQ(g.discover(), 0);
  auto order = order_of(g);
  CHECK_EQ(order.size(), size_t{3});
  CHECK(index_of(order, "c") < index_of(order, "b"));
  CHECK(index_of(order, "b") < index_of(order, "a"));
  fs::remove_all(d);
}

void test_diamond() {
  auto d = make_tmpdir("diamond");
  // Diamond dependency shape: top depends on b and c; both depend on base.
  write_unit(d, "base", "");
  write_unit(d, "b", "base");
  write_unit(d, "c", "base");
  write_unit(d, "top", "b, c");
  UnitGraph g;
  g.add_search_root(d);
  CHECK_EQ(g.discover(), 0);
  auto order = order_of(g);
  CHECK_EQ(order.size(), size_t{4});
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
  UnitGraph g;
  g.add_search_root(d);
  CHECK_EQ(g.discover(), 0);
  auto order = order_of(g);
  CHECK_EQ(order.size(), size_t{2});
  CHECK(index_of(order, "alpha") < index_of(order, "beta"));
  fs::remove_all(d);
}

void test_external_uses_ignored() {
  // If a unit references something not present in the graph (e.g. `dos`
  // from the RTL), it should not create a phantom cycle.
  auto d = make_tmpdir("external");
  write_unit(d, "solo", "dos, strings, linux");
  UnitGraph g;
  g.add_search_root(d);
  CHECK_EQ(g.discover(), 0);
  auto tr = g.topo_sort();
  CHECK_EQ(tr.order.size(), size_t{1});
  CHECK_EQ(tr.order[0], std::string("solo"));
  CHECK(tr.cycle_edges.empty());
  fs::remove_all(d);
}

void test_cycle_detected() {
  auto d = make_tmpdir("cycle");
  write_unit(d, "a", "b");
  write_unit(d, "b", "a");
  UnitGraph g;
  g.add_search_root(d);
  CHECK_EQ(g.discover(), 0);
  auto tr = g.topo_sort();
  // Neither unit should be in order; both are on a cycle.
  CHECK(tr.order.empty());
  CHECK(!tr.cycle_edges.empty());
  fs::remove_all(d);
}

void test_skip_path() {
  auto d = make_tmpdir("skip");
  write_unit(d, "keep", "");
  write_unit(d, "drop", "");
  UnitGraph g;
  g.add_search_root(d);
  g.skip_path_containing("/drop.pas");
  CHECK_EQ(g.discover(), 0);
  CHECK_EQ(g.units().size(), size_t{1});
  CHECK(g.lookup("keep") != nullptr);
  CHECK(g.lookup("drop") == nullptr);
  fs::remove_all(d);
}

void test_discover_from_entry_only_reachable_units() {
  auto d = make_tmpdir("entry");
  fs::create_directories(d / "sub");
  write_program(d / "main.pas", "main", "alpha");
  write_unit(d, "alpha", "beta");
  write_unit(d / "sub", "beta", "");
  write_unit(d, "unused", "");

  UnitGraph g;
  CHECK_EQ(g.discover_from_entry(d / "main.pas"), 0);
  CHECK_EQ(g.units().size(), size_t{3});
  CHECK(g.lookup("main") != nullptr);
  CHECK(g.lookup("alpha") != nullptr);
  CHECK(g.lookup("beta") != nullptr);
  CHECK(g.lookup("unused") == nullptr);
  fs::remove_all(d);
}

void test_real_fpc_compiler_acyclic() {
  // Smoke test: the real fpc 0.99 compiler source tree should be acyclic.
  fs::path src_root = fs::path(__FILE__).parent_path().parent_path().parent_path();
  fs::path compiler = src_root / "rpm" / "compiler";
  if (!fs::exists(compiler)) {
    // Skip silently if not run from the expected tree layout.
    return;
  }
  UnitGraph g;
  g.add_search_root(compiler);
  g.define("FPC");
  g.define("I386");
  g.define("LINUX");
  g.skip_path_containing("/new/");
  g.skip_path_containing("/tokendat.pas");
  CHECK_EQ(g.discover(), 0);
  auto tr = g.topo_sort();
  CHECK(tr.cycle_edges.empty());
  CHECK(g.units().size() > 100);  // ~129 units
  CHECK_EQ(tr.order.size(), g.units().size());
}

}  // namespace

int main() {
  RUN_TEST(test_single_unit);
  RUN_TEST(test_linear_chain);
  RUN_TEST(test_diamond);
  RUN_TEST(test_case_insensitive);
  RUN_TEST(test_external_uses_ignored);
  RUN_TEST(test_cycle_detected);
  RUN_TEST(test_skip_path);
  RUN_TEST(test_discover_from_entry_only_reachable_units);
  RUN_TEST(test_real_fpc_compiler_acyclic);

  int n = p2cc_test::failures();
  std::printf("%s: %d failure%s\n", (n == 0 ? "PASS" : "FAIL"), n,
              (n == 1 ? "" : "s"));
  return n == 0 ? 0 : 1;
}
