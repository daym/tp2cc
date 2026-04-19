#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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
using namespace p2cc;

namespace {

int cmd_lex(const std::string& path) {
  auto sf = SourceFile::load(path);
  if (!sf) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); return 2; }
  Lexer lex(std::move(sf));
  lex.define("FPC");
  lex.define("I386");
  lex.define("LINUX");
  for (;;) {
    Token t = lex.next();
    std::printf("%-4u:%-3u  %-3u  %s\n", t.loc.line, t.loc.col,
                static_cast<unsigned>(t.kind), t.text.c_str());
    if (t.kind == Tok::Eof) break;
  }
  return error_count() == 0 ? 0 : 1;
}

// Files we never translate and therefore never try to lex:
//   - tokendat.pas:  a TP7-only build utility; its own {$fatal} rejects FPC.
//     Not part of the compiler proper.
static bool is_skipped(const std::string& path) {
  if (path.find("/new/") != std::string::npos) return true;  // newer WIP tree
  if (path.find("/tokendat.pas") != std::string::npos) return true;
  return false;
}

int cmd_lex_all(const std::string& dir) {
  int fails = 0;
  int ok = 0;
  for (auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    auto ext = e.path().extension();
    if (ext != ".pas" && ext != ".pp" && ext != ".inc") continue;
    auto s = e.path().string();
    if (is_skipped(s)) continue;

    auto sf = SourceFile::load(e.path());
    if (!sf) { ++fails; continue; }
    int errs_before = error_count();
    Lexer lex(std::move(sf));
    lex.define("FPC");
    lex.define("I386");
    lex.define("LINUX");
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
      std::printf("FAIL %s (%d errors, %d tokens)\n", s.c_str(), errs, tokens);
      ++fails;
    }
  }
  std::printf("lex-all: %d ok, %d failed\n", ok, fails);
  return fails == 0 ? 0 : 1;
}

int cmd_parse(const std::string& path) {
  auto sf = SourceFile::load(path);
  if (!sf) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); return 2; }
  Lexer lex(std::move(sf));
  lex.define("FPC");
  lex.define("I386");
  lex.define("LINUX");
  Parser parser(lex);
  auto u = parser.parse();
  if (!u) return 1;
  if (error_count() > 0) return 1;
  std::printf("parse ok: %s '%s'\n",
              u->is_program ? "program" : "unit", u->name.c_str());
  return 0;
}

int cmd_parse_all(const std::string& dir) {
  int fails = 0;
  int ok = 0;
  for (auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    auto ext = e.path().extension();
    if (ext != ".pas" && ext != ".pp") continue;   // skip .inc here
    auto s = e.path().string();
    if (is_skipped(s)) continue;

    auto sf = SourceFile::load(e.path());
    if (!sf) { ++fails; continue; }
    int errs_before = error_count();
    Lexer lex(std::move(sf));
    lex.define("FPC");
    lex.define("I386");
    lex.define("LINUX");
    Parser parser(lex);
    auto u = parser.parse();
    int errs = error_count() - errs_before;
    if (errs == 0 && u) {
      ++ok;
    } else {
      std::printf("FAIL %s (%d errors)\n", s.c_str(), errs);
      ++fails;
    }
  }
  std::printf("parse-all: %d ok, %d failed\n", ok, fails);
  return fails == 0 ? 0 : 1;
}

int cmd_emit_all(const std::string& in_dir, const std::string& outdir) {
  UnitGraph g;
  g.add_search_root(in_dir);
  g.define("FPC");
  g.define("I386");
  g.define("LINUX");
  g.skip_path_containing("/new/");
  g.skip_path_containing("/tokendat.pas");
  // ppovin.pas is TP7-only overlay glue (pp.pas does
  // `{$ifdef FPC}{$UNDEF USEOVERLAY}{$ENDIF}`; under FPC it's never
  // `uses`d). Emitting it independently drags in an undeclared
  // `ovrgetbuf` -- skip it entirely.
  g.skip_path_containing("/ppovin.pas");
  // Non-Linux target-specific units. We're emitting for linux/i386,
  // so dos-extender / os2 / win32 back-ends are dead code for the
  // bootstrap path.
  g.skip_path_containing("/t_go32v1.pas");
  g.skip_path_containing("/t_go32v2.pas");
  g.skip_path_containing("/t_os2.pas");
  g.skip_path_containing("/t_win32.pas");
  g.skip_path_containing("/m68k/");
  g.skip_path_containing("/alpha/");
  g.skip_path_containing("/powerpc/");
  // We target i386. Non-i386 CPU back-end modules reference registers
  // (R_D0 etc.) defined only in their own cpubase; on i386 they are dead
  // code. Skip 68k code generator + assembler files in the compiler root.
  for (const char* p : {
           "/cg68k",   "/ag68k",  "/cga68k",  "/tgen68k",
           "/opts68k", "/ra68k",  "/og68k"}) {
    g.skip_path_containing(p);
  }
  int derr = g.discover();
  if (derr) std::fprintf(stderr, "discover saw %d errors\n", derr);
  auto tr = g.topo_sort();
  if (!tr.cycle_edges.empty()) {
    std::fprintf(stderr, "cycle in interface-uses; aborting\n");
    return 1;
  }
  fs::create_directories(outdir);
  int emitted = 0, failed = 0;
  std::set<std::string> rtl_refs;  // external-RTL unit names seen in uses

  // Pre-pass: collect cross-unit name tables so the emitter can
  // auto-parenthesise method calls correctly across unit boundaries
  // and compute array dimensions indexed by enums from other units.
  std::unordered_set<std::string> all_parameterless;
  std::unordered_set<std::string> all_fields;
  std::unordered_map<std::string, EnumInfo> all_enums;
  for (const auto& [_, pu] : g.units()) {
    if (pu.ast) {
      collect_parameterless_methods(*pu.ast, all_parameterless);
      collect_field_names(*pu.ast, all_fields);
      collect_enum_sizes(*pu.ast, all_enums);
    }
  }

  // Reified type/symbol tree spanning every parsed unit. The emitter
  // consults it to decide, per Pascal-level semantics, whether an
  // expression `obj.name` refers to a field or a parameterless method
  // (and similarly for bare identifier auto-call).
  std::vector<const ast::UnitNode*> asts;
  for (const auto& [_, pu] : g.units()) {
    if (pu.ast) asts.push_back(pu.ast.get());
  }
  TypeRegistry reg;
  reg.build(asts);

  for (const auto& name : tr.order) {
    const auto* pu = g.lookup(name);
    if (!pu || !pu->ok || !pu->ast) { ++failed; continue; }
    auto out = emit_unit(*pu->ast, all_parameterless, all_fields,
                         all_enums, &reg);
    {
      std::ofstream h(fs::path(outdir) / ("p_" + name + ".h"));
      h << out.header;
    }
    {
      std::ofstream c(fs::path(outdir) / ("p_" + name + ".cc"));
      c << out.impl;
    }
    // Collect external (not in the graph) uses names.
    auto scan = [&](const std::vector<std::string>& uses) {
      for (const auto& u : uses) {
        std::string lu = u;
        for (auto& ch : lu) {
          if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        if (!g.lookup(lu)) rtl_refs.insert(lu);
      }
    };
    scan(pu->ast->interface_uses);
    scan(pu->ast->impl_uses);
    ++emitted;
  }
  // Emit stub headers for external RTL units so `#include "unit.h"` resolves.
  // They declare an empty namespace with the expected name; real symbols come
  // from p2cc_rt/prelude.h (or are added later as the runtime grows).
  for (const auto& u : rtl_refs) {
    std::ofstream h(fs::path(outdir) / ("p_" + u + ".h"));
    h << "// p2cc: RTL stub for external unit '" << u << "'.\n";
    h << "#pragma once\n";
    h << "#include \"p2cc_rt/prelude.h\"\n";
    h << "namespace p_" << u << " { using namespace ::rt; }\n";
  }
  std::printf("emit-all: %d emitted, %d failed (of %zu units), "
              "%zu rtl stubs\n",
              emitted, failed, g.units().size(), rtl_refs.size());
  return failed == 0 ? 0 : 1;
}

int cmd_emit(const std::string& path, const std::string& outdir) {
  auto sf = SourceFile::load(path);
  if (!sf) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); return 2; }
  Lexer lex(std::move(sf));
  lex.define("FPC");
  lex.define("I386");
  lex.define("LINUX");
  Parser parser(lex);
  auto u = parser.parse();
  if (!u || error_count() > 0) return 1;
  auto out = emit_unit(*u);
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

int cmd_topo(const std::string& dir) {
  UnitGraph g;
  g.add_search_root(dir);
  g.define("FPC");
  g.define("I386");
  g.define("LINUX");
  g.skip_path_containing("/new/");
  g.skip_path_containing("/tokendat.pas");
  // Non-i386 CPU subtrees carry units with the same names as the i386
  // versions (e.g. cpubase.pas); skip them so the discovery map is clean.
  g.skip_path_containing("/m68k/");
  g.skip_path_containing("/alpha/");
  g.skip_path_containing("/powerpc/");
  int errs = g.discover();
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
               "  p2cc lex <file>\n"
               "  p2cc lex-all <dir>\n"
               "  p2cc parse <file>\n"
               "  p2cc parse-all <dir>\n"
               "  p2cc topo <dir>\n"
               "  p2cc emit <file> <outdir>\n"
               "  p2cc emit-all <dir> <outdir>\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { usage(); return 2; }
  std::string cmd = argv[1];
  std::string arg = argv[2];
  if (cmd == "lex") return cmd_lex(arg);
  if (cmd == "lex-all") return cmd_lex_all(arg);
  if (cmd == "parse") return cmd_parse(arg);
  if (cmd == "parse-all") return cmd_parse_all(arg);
  if (cmd == "topo") return cmd_topo(arg);
  if (cmd == "emit") {
    if (argc < 4) { usage(); return 2; }
    return cmd_emit(arg, argv[3]);
  }
  if (cmd == "emit-all") {
    if (argc < 4) { usage(); return 2; }
    return cmd_emit_all(arg, argv[3]);
  }
  usage();
  return 2;
}
