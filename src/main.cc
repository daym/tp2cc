#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fstream>
#include <set>
#include <unordered_set>

#include "diag.h"
#include "emit.h"
#include "lexer.h"
#include "parser.h"
#include "source.h"
#include "typereg.h"
#include "units.h"

namespace fs = std::filesystem;
using namespace tp2cc;

namespace {

std::string to_lower(std::string_view s) {
  std::string r(s);
  for (auto& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return r;
}

struct CliOptions {
  std::vector<std::string> defines;
  std::vector<fs::path> unit_paths;     // -Fu<dir>
  std::vector<fs::path> include_paths;  // -Fi<dir>
};

// Reserved extension point: if tp2cc ever needs to predefine symbols
// unconditionally, add them here. Callers are expected to pass their
// own set via -dSYMBOL flags.
void define_default_symbols(Lexer&) {}
void define_default_symbols(UnitGraph&) {}

std::unique_ptr<Lexer> make_lexer(const std::string& path,
                                  const CliOptions& opts) {
  auto sf = SourceFile::load(path);
  if (!sf) return nullptr;
  auto lex = std::make_unique<Lexer>(std::move(sf), opts.include_paths);
  define_default_symbols(*lex);
  for (const auto& d : opts.defines) lex->define(d);
  return lex;
}

void configure_graph(UnitGraph& g, const CliOptions& opts) {
  define_default_symbols(g);
  for (const auto& d : opts.defines) g.define(d);
  for (const auto& p : opts.unit_paths) g.add_search_root(p);
  for (const auto& p : opts.include_paths) g.add_include_path(p);
}

void visit_unit_init_dep(const UnitGraph& g,
                         const std::string& dep,
                         std::vector<std::string>* out,
                         std::unordered_set<std::string>* active_stack,
                         std::unordered_set<std::string>* emitted) {
  std::string name = to_lower(dep);
  const auto* pu = g.lookup(name);
  if (!pu || !pu->ok || !pu->ast || pu->ast->is_program) return;
  if (emitted->count(name)) return;

  // Implementation uses may legitimately cycle. Unit startup still walks
  // them because those units can carry initialization/finalization blocks,
  // but a back-edge only means "this unit is already being expanded"; it
  // must not recurse forever and it must still be emitted once afterward.
  if (!active_stack->insert(name).second) return;

  for (const auto& inner : pu->ast->interface_uses) {
    visit_unit_init_dep(g, inner, out, active_stack, emitted);
  }
  for (const auto& inner : pu->ast->impl_uses) {
    visit_unit_init_dep(g, inner, out, active_stack, emitted);
  }
  active_stack->erase(name);

  if (emitted->insert(name).second) out->push_back(name);
}

void collect_unit_init_order(const UnitGraph& g,
                             const ast::UnitNode& u,
                             std::vector<std::string>* out,
                             std::unordered_set<std::string>* active_stack,
                             std::unordered_set<std::string>* emitted) {
  // Unit startup follows recursive uses-order, not the generic
  // translation-unit topo order used for file emission.
  for (const auto& dep : u.interface_uses) {
    visit_unit_init_dep(g, dep, out, active_stack, emitted);
  }
  for (const auto& dep : u.impl_uses) {
    visit_unit_init_dep(g, dep, out, active_stack, emitted);
  }
}

void write_external_stub(std::ostream& h, std::string_view unit_name) {
  h << "// tp2cc: RTL stub for external unit '" << unit_name << "'.\n";
  h << "#pragma once\n";
  h << "#include \"tp2cc_rt/prelude.h\"\n";

  if (unit_name == "sysutils") {
    h << "namespace p_sysutils {\n";
    h << "using p_exception = ::rt::p_exception;\n";
    h << "using tp2cc_metaclass_p_exception = "
         "::rt::tp2cc_metaclass_p_exception;\n";
    h << "inline const tp2cc_metaclass_p_exception* "
         "tp2cc_metaclass_value_p_exception() {\n";
    h << "  return ::rt::tp2cc_metaclass_value_p_exception();\n";
    h << "}\n";
    h << "\n";
    h << "// Re-export the Pascal-visible SysUtils surface that the compiler\n";
    h << "// reaches through qualified unit names when SysUtils itself stays an\n";
    h << "// external RTL stub.\n";
    static constexpr const char* kSysutilsTypeAliases[] = {
        "p_tdatetime",
        "p_tsystemtime",
        "p_pansistring",
    };
    for (const char* name : kSysutilsTypeAliases) {
      h << "using " << name << " = ::rt::" << name << ";\n";
    }
    static constexpr const char* kSysutilsValueAliases[] = {
        "p_changefileext",
        "p_getenvironmentvariable",
        "p_expandfilename",
        "p_executeprocess",
        "p_getlocaltime",
        "p_decodetime",
        "p_decodedate",
        "p_filedatetodatetime",
        "p_time",
        "p_date",
        "p_fileexists",
        "p_directoryexists",
        "p_renamefile",
        "p_filegetdate",
        "p_filesetdate",
        "p_fileage",
        "p_getfilehandle",
        "p_stringofchar",
        "p_ansicomparefilename",
    };
    for (const char* name : kSysutilsValueAliases) {
      h << "using ::rt::" << name << ";\n";
    }
    h << "\n";
    h << "// Compiler units catch a small SysUtils exception hierarchy even\n";
    h << "// when the full SysUtils unit is not translated. The exception\n";
    h << "// classes themselves live in `::rt::` (so checked-arith helpers\n";
    h << "// can throw them); alias each into `p_sysutils` so translated\n";
    h << "// code that says `on E: SysUtils.EIntOverflow do` resolves to\n";
    h << "// the same C++ class the runtime throws.\n";
    static constexpr const char* kRtAliasedExceptionClasses[] = {
        "p_eexternal",
        "p_einterror",
        "p_eintoverflow",
        "p_erangeerror",
        "p_edivbyzero",
    };
    for (const char* name : kRtAliasedExceptionClasses) {
      h << "using " << name << " = ::rt::" << name << ";\n";
    }
    // EOSError carries an `errorcode` field that the rt-side base class
    // does not provide; keep it as a stub redeclaration here.
    h << "struct p_eoserror : public p_exception {\n";
    h << "  using inherited = p_exception;\n";
    h << "  using inherited::p_create;\n";
    h << "  int32_t p_errorcode = 0;\n";
    h << "};\n";
    h << "}  // namespace p_sysutils\n";
    return;
  }

  h << "namespace rt {}\n";
  h << "namespace p_" << unit_name << " = ::rt;\n";
}

int cmd_lex(const CliOptions& opts,
            const std::vector<std::string>& files) {
  if (files.empty()) { std::fprintf(stderr, "lex: no input files\n"); return 2; }
  int fails = 0;
  for (const auto& path : files) {
    auto lex = make_lexer(path, opts);
    if (!lex) {
      std::fprintf(stderr, "cannot read %s\n", path.c_str());
      ++fails;
      continue;
    }
    for (;;) {
      Token t = lex->next();
      std::printf("%s  %-4u:%-3u  %-3u  %s\n", path.c_str(), t.loc.line,
                  t.loc.col, static_cast<unsigned>(t.kind), t.text.c_str());
      if (t.kind == Tok::Eof) break;
    }
  }
  return (fails == 0 && error_count() == 0) ? 0 : 1;
}

int cmd_lex_all(const CliOptions& opts,
                const std::vector<std::string>& files) {
  if (files.empty()) {
    std::fprintf(stderr, "lex-all: no input files\n");
    return 2;
  }
  int fails = 0;
  int ok = 0;
  for (const auto& path : files) {
    auto sf = SourceFile::load(path);
    if (!sf) { ++fails; std::printf("FAIL %s (cannot read)\n", path.c_str()); continue; }
    int errs_before = error_count();
    Lexer lex(std::move(sf), opts.include_paths);
    define_default_symbols(lex);
    for (const auto& d : opts.defines) lex.define(d);
    int tokens = 0;
    for (;;) {
      Token t = lex.next();
      ++tokens;
      if (t.kind == Tok::Eof) break;
      if (tokens > 10'000'000) break;
    }
    int errs = error_count() - errs_before;
    if (errs == 0) {
      ++ok;
    } else {
      std::printf("FAIL %s (%d errors, %d tokens)\n", path.c_str(), errs,
                  tokens);
      ++fails;
    }
  }
  std::printf("lex-all: %d ok, %d failed\n", ok, fails);
  return fails == 0 ? 0 : 1;
}

int cmd_parse(const CliOptions& opts,
              const std::vector<std::string>& files) {
  if (files.empty()) { std::fprintf(stderr, "parse: no input files\n"); return 2; }
  int fails = 0;
  for (const auto& path : files) {
    auto lex = make_lexer(path, opts);
    if (!lex) {
      std::fprintf(stderr, "cannot read %s\n", path.c_str());
      ++fails;
      continue;
    }
    Parser parser(*lex);
    auto u = parser.parse();
    if (!u || error_count() > 0) { ++fails; continue; }
    std::printf("parse ok: %s '%s'\n",
                u->is_program ? "program" : "unit", u->name.c_str());
  }
  return fails == 0 ? 0 : 1;
}

int cmd_parse_all(const CliOptions& opts,
                  const std::vector<std::string>& files) {
  if (files.empty()) {
    std::fprintf(stderr, "parse-all: no input files\n");
    return 2;
  }
  int fails = 0;
  int ok = 0;
  for (const auto& path : files) {
    auto sf = SourceFile::load(path);
    if (!sf) { ++fails; std::printf("FAIL %s (cannot read)\n", path.c_str()); continue; }
    int errs_before = error_count();
    Lexer lex(std::move(sf), opts.include_paths);
    define_default_symbols(lex);
    for (const auto& d : opts.defines) lex.define(d);
    Parser parser(lex);
    auto u = parser.parse();
    int errs = error_count() - errs_before;
    if (errs == 0 && u) {
      ++ok;
    } else {
      std::printf("FAIL %s (%d errors)\n", path.c_str(), errs);
      ++fails;
    }
  }
  std::printf("parse-all: %d ok, %d failed\n", ok, fails);
  return fails == 0 ? 0 : 1;
}

int cmd_emit_all(const CliOptions& opts, const std::string& input_path,
                 const std::string& outdir) {
  UnitGraph g;
  configure_graph(g, opts);
  // version.pas guards `source_cpu_string` on CPU86, which is implied by
  // I386. Keep it hardcoded so callers don't have to know this internal
  // detail.
  g.define("CPU86");
  // Skip compiling the huge msgtxt.inc message-text table into the
  // binary. Under EXTERN_MSG the compiler loads `errore.msg` from disk
  // at startup instead. Avoids the awkward-to-translate `@msgtxt` on an
  // `array[N] of string[240]` in verbose.pas:488. Build-model decision,
  // not a caller-tunable.
  g.define("EXTERN_MSG");
  fs::path input = input_path;
  if (!fs::is_regular_file(input)) {
    std::fprintf(stderr, "emit-all: not a regular file: %s\n",
                 input_path.c_str());
    return 2;
  }
  int derr = g.discover_from_entry(input);
  if (derr) std::fprintf(stderr, "discover saw %d errors\n", derr);
  auto tr = g.topo_sort();
  if (!tr.cycle_edges.empty()) {
    std::fprintf(stderr, "cycle in interface-uses; aborting\n");
    return 1;
  }
  fs::create_directories(outdir);
  int emitted = 0, failed = 0;
  std::set<std::string> rtl_refs;

  std::vector<const ast::UnitNode*> asts;
  for (const auto& [_, pu] : g.units()) {
    if (pu.ast) asts.push_back(pu.ast.get());
  }
  TypeRegistry reg;
  reg.build(asts);

  for (const auto& name : tr.order) {
    const auto* pu = g.lookup(name);
    if (!pu || !pu->ok || !pu->ast) { ++failed; continue; }
    const std::vector<std::string>* init_order = nullptr;
    std::vector<std::string> init_list;
    if (pu->ast->is_program) {
      std::unordered_set<std::string> visiting;
      std::unordered_set<std::string> emitted_units;
      collect_unit_init_order(g, *pu->ast, &init_list, &visiting,
                              &emitted_units);
      init_order = &init_list;
    }
    int errs_before = error_count();
    auto out = emit_unit(*pu->ast, &reg, init_order);
    int errs = error_count() - errs_before;
    if (errs != 0) {
      std::printf("FAIL %s (%d emit errors)\n", name.c_str(), errs);
      ++failed;
      continue;
    }
    {
      std::ofstream h(fs::path(outdir) / ("p_" + name + ".h"));
      h << out.header;
    }
    {
      std::ofstream c(fs::path(outdir) / ("p_" + name + ".cc"));
      c << out.impl;
    }
    auto scan = [&](const std::vector<std::string>& uses) {
      for (const auto& u : uses) {
        std::string lu = to_lower(u);
        if (!g.lookup(lu)) rtl_refs.insert(lu);
      }
    };
    scan(pu->ast->interface_uses);
    scan(pu->ast->impl_uses);
    ++emitted;
  }
  for (const auto& u : rtl_refs) {
    std::ofstream h(fs::path(outdir) / ("p_" + u + ".h"));
    write_external_stub(h, u);
  }
  std::printf("emit-all: %d emitted, %d failed (of %zu units), "
              "%zu rtl stubs\n",
              emitted, failed, g.units().size(), rtl_refs.size());
  return failed == 0 ? 0 : 1;
}

int cmd_emit(const CliOptions& opts, const std::string& path,
             const std::string& outdir) {
  auto lex = make_lexer(path, opts);
  if (!lex) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); return 2; }
  Parser parser(*lex);
  auto u = parser.parse();
  if (!u || error_count() > 0) return 1;
  int errs_before = error_count();
  auto out = emit_unit(*u);
  if (error_count() != errs_before) return 1;
  fs::create_directories(outdir);
  std::string stem = u->name;
  if (stem.empty()) stem = fs::path(path).stem().string();
  {
    std::ofstream h(fs::path(outdir) / ("p_" + stem + ".h"));
    h << out.header;
  }
  {
    std::ofstream c(fs::path(outdir) / ("p_" + stem + ".cc"));
    c << out.impl;
  }
  std::printf("emitted p_%s.h and p_%s.cc\n", stem.c_str(), stem.c_str());
  return 0;
}

int cmd_topo(const CliOptions& opts,
             const std::vector<std::string>& files) {
  if (files.size() != 1) {
    std::fprintf(stderr, "topo: expected exactly one entry file\n");
    return 2;
  }
  UnitGraph g;
  configure_graph(g, opts);
  fs::path input = files[0];
  if (!fs::is_regular_file(input)) {
    std::fprintf(stderr, "topo: not a regular file: %s\n", files[0].c_str());
    return 2;
  }
  int errs = g.discover_from_entry(input);
  auto tr = g.topo_sort();
  std::printf("units discovered: %zu\n", g.units().size());
  std::printf("topo order:\n");
  for (const auto& n : tr.order) std::printf("  %s\n", n.c_str());
  if (!tr.cycle_edges.empty()) {
    std::printf("cycles:\n");
    for (const auto& [a, b] : tr.cycle_edges) {
      std::printf("  %s -> %s\n", a.c_str(), b.c_str());
    }
  }
  return (errs == 0 && tr.cycle_edges.empty()) ? 0 : 1;
}

void usage() {
  std::fprintf(stderr,
    "usage:\n"
    "  tp2cc <subcommand> [-h] [-dSYMBOL]... [-Fu<dir>]... [-Fi<dir>]... "
    "[--] <args>...\n"
    "\n"
    "subcommands:\n"
    "  lex <file>...                  tokenize each file, dump tokens\n"
    "  lex-all <file>...              tokenize each file, summary only\n"
    "  parse <file>...                parse each file\n"
    "  parse-all <file>...            parse each file, summary only\n"
    "  topo <entry.pas>               walk `uses`, print topo order\n"
    "  emit <file> <outdir>           translate a single unit\n"
    "  emit-all <entry.pas> <outdir>  translate entry + uses tree\n"
    "\n"
    "options (anywhere after <subcommand>, interleavable with positional):\n"
    "  -h, --help   show this help and exit\n"
    "  -dSYMBOL     predefine SYMBOL for {$ifdef}\n"
    "  -Fu<dir>     add <dir> to unit search path (for `uses`)\n"
    "  -Fi<dir>     add <dir> to include search path (for {$I})\n"
    "  --           end of options (subsequent args are positional)\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { usage(); return 2; }
  std::string cmd = argv[1];
  if (cmd == "-h" || cmd == "--help") { usage(); return 0; }

  CliOptions opts;
  std::vector<std::string> positional;
  bool end_of_opts = false;
  for (int i = 2; i < argc; ++i) {
    std::string_view a = argv[i];
    if (!end_of_opts) {
      if (a == "--") { end_of_opts = true; continue; }
      if (a == "-h" || a == "--help") { usage(); return 0; }
      if (a.size() > 2 && a[0] == '-' && a[1] == 'd') {
        opts.defines.emplace_back(a.substr(2));
        continue;
      }
      if (a.size() > 3 && a[0] == '-' && a[1] == 'F' && a[2] == 'u') {
        opts.unit_paths.emplace_back(std::string(a.substr(3)));
        continue;
      }
      if (a.size() > 3 && a[0] == '-' && a[1] == 'F' && a[2] == 'i') {
        opts.include_paths.emplace_back(std::string(a.substr(3)));
        continue;
      }
      if (!a.empty() && a[0] == '-') {
        std::fprintf(stderr, "unknown option: %s\n", argv[i]);
        usage();
        return 2;
      }
    }
    positional.emplace_back(argv[i]);
  }

  if (cmd == "lex")       return cmd_lex(opts, positional);
  if (cmd == "lex-all")   return cmd_lex_all(opts, positional);
  if (cmd == "parse")     return cmd_parse(opts, positional);
  if (cmd == "parse-all") return cmd_parse_all(opts, positional);
  if (cmd == "topo")      return cmd_topo(opts, positional);
  if (cmd == "emit") {
    if (positional.size() != 2) { usage(); return 2; }
    return cmd_emit(opts, positional[0], positional[1]);
  }
  if (cmd == "emit-all") {
    if (positional.size() != 2) { usage(); return 2; }
    return cmd_emit_all(opts, positional[0], positional[1]);
  }
  std::fprintf(stderr, "unknown subcommand: %s\n", cmd.c_str());
  usage();
  return 2;
}
