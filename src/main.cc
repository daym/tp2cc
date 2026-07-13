#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fstream>
#include <set>
#include <unordered_set>
#include <unistd.h>

#include "diag.h"
#include "emit.h"
#include "emit_makefile.h"
#include "lexer.h"
#include "parser.h"
#include "registry_parser_actions.h"
#include "runtime_units.h"
#include "source.h"
#include "typereg.h"
#include "units.h"
#include "target_info.h"

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

TargetInfo target_info_for_cpu(std::string_view cpu) {
  if (cpu.empty()) return default_target_info();
  std::string lower(cpu);
  for (auto& c : lower) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  // 32-bit CPUs
  if (lower == "i386" || lower == "m68k" || lower == "powerpc" ||
      lower == "sparc" || lower == "mips" || lower == "mipsel" ||
      lower == "arm" || lower == "i8086" || lower == "avr" ||
      lower == "vm" || lower == "jvm" || lower == "wasm") {
    return {.pointer_bits = 32};
  }
  // 64-bit CPUs
  if (lower == "x86_64" || lower == "aarch64" || lower == "powerpc64" ||
      lower == "sparc64" || lower == "ia64" || lower == "alpha") {
    return {.pointer_bits = 64};
  }
  std::fprintf(stderr,
               "unsupported target CPU: %s\n"
               "supported: i386 m68k powerpc sparc mips mipsel arm i8086 avr "
               "vm jvm wasm x86_64 aarch64 powerpc64 sparc64 ia64 alpha\n",
               std::string(cpu).c_str());
  std::exit(2);
}

struct CliOptions {
  std::vector<std::string> defines;
  std::vector<fs::path> unit_paths;     // -Fu<dir>
  std::vector<fs::path> include_paths;  // -Fi<dir>
  std::vector<std::string> regen_cli_args;
  std::string invoked_as;
  // Pascal `{$Q+}` / `{$R+}` initial state. Mirrors fpc's `-Co` /
  // `-Cr` cmdline flags. Source-level directives override.
  bool overflow_check = false;
  bool range_check = false;
  bool emit_makefile = false;
  std::string cpu;  // -P<cpu>: target CPU
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
  lex->set_overflow_check_default(opts.overflow_check);
  lex->set_range_check_default(opts.range_check);
  return lex;
}

void configure_graph(UnitGraph& g, const CliOptions& opts) {
  define_default_symbols(g);
  for (const auto& d : opts.defines) g.define(d);
  g.set_overflow_check_default(opts.overflow_check);
  g.set_range_check_default(opts.range_check);
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

void write_runtime_unit_shim(std::ostream& h, const UnitInfo& unit) {
  h << "// tp2cc: runtime-backed Pascal unit '" << unit.name << "'.\n";
  h << "#pragma once\n";
  h << "#include \"tp2cc_rt/prelude.h\"\n";

  std::set<std::string> value_names;
  for (const auto& [name, _] : unit.iface_procs) value_names.insert(name);
  for (const auto& [name, _] : unit.iface_vars) value_names.insert(name);
  for (const auto& [name, _] : unit.iface_consts) value_names.insert(name);

  h << "namespace p_" << unit.name << " {\n";
  for (const std::string& name : value_names) {
    h << "using ::rt::p_" << name << ";\n";
  }
  h << "}  // namespace p_" << unit.name << "\n";
}

std::vector<std::string> runtime_unit_refs_in_uses(
    const UnitGraph& g, const std::vector<std::string>& uses) {
  std::vector<std::string> refs;
  for (const auto& unit_name : uses) {
    std::string canonical = to_lower(unit_name);
    if (!g.lookup(canonical) && runtime_unit_model(canonical)) {
      refs.push_back(std::move(canonical));
    }
  }
  return refs;
}

std::optional<fs::path> resolve_tp2cc_program(std::string_view invoked_as) {
  if (invoked_as.empty()) return std::nullopt;
  std::error_code ec;
  fs::path raw(invoked_as);
  if (raw.is_absolute() || raw.has_parent_path()) {
    fs::path abs = fs::absolute(raw, ec);
    if (ec) return raw.lexically_normal();
    return abs.lexically_normal();
  }

  if (const char* path_env = std::getenv("PATH")) {
    std::string_view path_list(path_env);
    size_t start = 0;
    while (start <= path_list.size()) {
      size_t end = path_list.find(':', start);
      std::string_view elem = path_list.substr(
          start, end == std::string_view::npos ? std::string_view::npos
                                               : end - start);
      fs::path dir = elem.empty() ? fs::current_path(ec) : fs::path(elem);
      if (!ec) {
        fs::path candidate = dir / raw;
        if (::access(candidate.c_str(), X_OK) == 0) {
          fs::path abs = fs::absolute(candidate, ec);
          if (ec) return candidate.lexically_normal();
          return abs.lexically_normal();
        }
      }
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
  }
  return std::nullopt;
}

std::string make_tp2cc_program(std::string_view invoked_as,
                               const fs::path& outdir) {
  if (invoked_as.empty()) return "tp2cc";
  fs::path raw(invoked_as);
  if (!raw.is_absolute() && !raw.has_parent_path()) {
    return std::string(invoked_as);
  }
  auto resolved = resolve_tp2cc_program(invoked_as);
  if (!resolved) return std::string(invoked_as);
  std::error_code ec;
  fs::path out_abs = fs::absolute(outdir, ec);
  if (ec) return resolved->generic_string();
  fs::path rel = fs::relative(*resolved, out_abs, ec);
  if (ec || rel.empty()) return resolved->generic_string();
  return rel.generic_string();
}

std::vector<std::string> make_tp2cc_include_dirs(std::string_view invoked_as,
                                                 const fs::path& outdir) {
  std::vector<std::string> dirs;
  auto resolved = resolve_tp2cc_program(invoked_as);
  if (!resolved) return dirs;

  std::error_code ec;
  fs::path out_abs = fs::absolute(outdir, ec);
  if (ec) return dirs;

  fs::path bin_dir = resolved->parent_path();
  const fs::path candidates[] = {bin_dir / ".." / "include",
                                 bin_dir / ".." / ".." / "include"};
  for (const fs::path& candidate : candidates) {
    fs::path normalized = candidate.lexically_normal();
    fs::path rel = fs::relative(normalized, out_abs, ec);
    if (ec || rel.empty()) {
      ec.clear();
      dirs.push_back(normalized.generic_string());
    } else {
      dirs.push_back(rel.generic_string());
    }
  }
  return dirs;
}

std::vector<std::string> make_regen_tp2cc_args(const std::string& subcommand,
                                               const CliOptions& opts,
                                               const fs::path& input_path) {
  std::vector<std::string> args;
  args.push_back(subcommand);
  args.insert(args.end(), opts.regen_cli_args.begin(), opts.regen_cli_args.end());
  args.push_back("--");
  args.push_back(fs::absolute(input_path).generic_string());
  args.push_back(".");
  return args;
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
                 const std::string& outdir, TargetInfo target) {
  UnitGraph g{};
  configure_graph(g, opts);
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
  std::vector<std::string> pas_sources;
  std::vector<std::string> cc_sources;
  std::vector<std::string> headers;
  std::string program_name;

  TypeRegistry& reg = g.type_registry();

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
    auto out = emit_unit(*pu->ast, reg, init_order, target);
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
    pas_sources.push_back(fs::absolute(pu->path).generic_string());
    cc_sources.push_back("p_" + name + ".cc");
    headers.push_back("p_" + name + ".h");
    if (pu->ast->is_program && program_name.empty()) {
      program_name = name;
    }
    for (auto ref : runtime_unit_refs_in_uses(g, pu->ast->interface_uses)) {
      rtl_refs.insert(std::move(ref));
    }
    for (auto ref : runtime_unit_refs_in_uses(g, pu->ast->impl_uses)) {
      rtl_refs.insert(std::move(ref));
    }
    ++emitted;
  }
  for (const auto& u : rtl_refs) {
    std::ofstream h(fs::path(outdir) / ("p_" + u + ".h"));
    auto runtime_unit = reg.units.find(u);
    if (runtime_unit == reg.units.end()) {
      report_error(Location{}, "internal error: runtime-backed unit `" + u +
                                   "` has no semantic model");
      ++failed;
      continue;
    }
    write_runtime_unit_shim(h, runtime_unit->second);
    headers.push_back("p_" + u + ".h");
  }
  if (opts.emit_makefile) {
    EmittedBuildManifest manifest{
        .cc_sources = std::move(cc_sources),
        .headers = std::move(headers),
        .pas_sources = std::move(pas_sources),
        .tp2cc_program = make_tp2cc_program(opts.invoked_as, fs::path(outdir)),
        .include_dirs = make_tp2cc_include_dirs(opts.invoked_as,
                                                fs::path(outdir)),
        .tp2cc_args = make_regen_tp2cc_args("emit-all", opts, input),
        .program_name = std::move(program_name)};
    std::ofstream mk(fs::path(outdir) / "Makefile");
    mk << emit_makefile(manifest);
  }
  std::printf("emit-all: %d emitted, %d failed (of %zu units), "
              "%zu runtime unit shims\n",
              emitted, failed, g.units().size(), rtl_refs.size());
  return failed == 0 ? 0 : 1;
}

int cmd_emit(const CliOptions& opts, const std::string& path,
             const std::string& outdir, TargetInfo target) {
  auto lex = make_lexer(path, opts);
  if (!lex) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); return 2; }
  TypeRegistry reg;
  RegistryParserActions semantic_actions(reg);
  Parser parser(*lex, &semantic_actions);
  auto u = parser.parse();
  if (!u || error_count() > 0) return 1;
  int errs_before = error_count();
  auto out = emit_unit(*u, reg, nullptr, target);
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
  if (opts.emit_makefile) {
    EmittedBuildManifest manifest{
        .cc_sources = {"p_" + stem + ".cc"},
        .headers = {"p_" + stem + ".h"},
        .pas_sources = {fs::absolute(path).generic_string()},
        .tp2cc_program = make_tp2cc_program(opts.invoked_as, fs::path(outdir)),
        .include_dirs = make_tp2cc_include_dirs(opts.invoked_as,
                                                fs::path(outdir)),
        .tp2cc_args = make_regen_tp2cc_args("emit", opts, path),
        .program_name = u->is_program ? stem : std::string{}};
    std::ofstream mk(fs::path(outdir) / "Makefile");
    mk << emit_makefile(manifest);
    std::printf("emitted p_%s.h, p_%s.cc, and Makefile\n",
                stem.c_str(), stem.c_str());
  } else {
    std::printf("emitted p_%s.h and p_%s.cc\n", stem.c_str(), stem.c_str());
  }
  return 0;
}

int cmd_topo(const CliOptions& opts,
             const std::vector<std::string>& files) {
  if (files.size() != 1) {
    std::fprintf(stderr, "topo: expected exactly one entry file\n");
    return 2;
  }
  UnitGraph g{};
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
    "  tp2cc <subcommand> [-h] [-m] [-dSYMBOL[:=TEXT]]... [-Fu<dir>]... [-Fi<dir>]... "
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
    "  -m           also emit a Makefile in the output directory\n"
    "  -dSYMBOL[:=TEXT]\n"
    "               predefine SYMBOL, optionally with text for {$if}\n"
    "  -P<cpu>     target CPU (i386, x86_64, arm, aarch64, powerpc,\n"
    "              powerpc64, mipsel, ...; determines pointer width)\n"
    "  -Fu<dir>     add <dir> to unit search path (for `uses`)\n"
    "  -Fi<dir>     add <dir> to include search path (for {$I})\n"
    "  --           end of options (subsequent args are positional)\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { usage(); return 2; }
  std::string cmd = argv[1];
  if (cmd == "-h" || cmd == "--help") { usage(); return 0; }

  std::vector<std::string> defines;
  std::vector<fs::path> unit_paths;
  std::vector<fs::path> include_paths;
  std::vector<std::string> regen_cli_args;
  bool overflow_check = false;
  bool range_check = false;
  bool emit_makefile = false;
  std::string cpu;
  std::vector<std::string> positional;
  bool end_of_opts = false;
  for (int i = 2; i < argc; ++i) {
    std::string_view a = argv[i];
    if (!end_of_opts) {
      if (a == "--") { end_of_opts = true; continue; }
      if (a == "-h" || a == "--help") { usage(); return 0; }
      if (a == "-m") { emit_makefile = true; continue; }
      if (a.size() > 2 && a[0] == '-' && a[1] == 'd') {
        defines.emplace_back(a.substr(2));
        regen_cli_args.emplace_back(argv[i]);
        continue;
      }
      if (a.size() > 3 && a[0] == '-' && a[1] == 'F' && a[2] == 'u') {
        unit_paths.emplace_back(std::string(a.substr(3)));
        regen_cli_args.emplace_back(argv[i]);
        continue;
      }
      if (a.size() > 3 && a[0] == '-' && a[1] == 'F' && a[2] == 'i') {
        include_paths.emplace_back(std::string(a.substr(3)));
        regen_cli_args.emplace_back(argv[i]);
        continue;
      }
      if (a == "-Co") {
        overflow_check = true;
        regen_cli_args.emplace_back(argv[i]);
        continue;
      }
      if (a == "-Cr") {
        range_check = true;
        regen_cli_args.emplace_back(argv[i]);
        continue;
      }
      if (a.size() > 2 && a[0] == '-' && a[1] == 'P') {
        cpu = std::string(a.substr(2));
        regen_cli_args.emplace_back(argv[i]);
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

  const CliOptions opts{.defines = std::move(defines),
                        .unit_paths = std::move(unit_paths),
                        .include_paths = std::move(include_paths),
                        .regen_cli_args = std::move(regen_cli_args),
                        .invoked_as = argv[0],
                        .overflow_check = overflow_check,
                        .range_check = range_check,
                        .emit_makefile = emit_makefile,
                        .cpu = cpu};

  if (cmd == "lex")       return cmd_lex(opts, positional);
  if (cmd == "lex-all")   return cmd_lex_all(opts, positional);
  if (cmd == "parse")     return cmd_parse(opts, positional);
  if (cmd == "parse-all") return cmd_parse_all(opts, positional);
  if (cmd == "topo")      return cmd_topo(opts, positional);
  if (cmd == "emit") {
    if (positional.size() != 2) { usage(); return 2; }
    TargetInfo target = target_info_for_cpu(cpu);
    return cmd_emit(opts, positional[0], positional[1], target);
  }
  if (cmd == "emit-all") {
    if (positional.size() != 2) { usage(); return 2; }
    TargetInfo target = target_info_for_cpu(cpu);
    return cmd_emit_all(opts, positional[0], positional[1], target);
  }
  std::fprintf(stderr, "unknown subcommand: %s\n", cmd.c_str());
  usage();
  return 2;
}
