#pragma once

#include <span>
#include <string_view>

namespace tp2cc {

enum class RuntimeUnitExportKind {
  Type,
  Proc,
  Var,
  Const,
};

struct RuntimeUnitExport {
  constexpr RuntimeUnitExport(RuntimeUnitExportKind k, std::string_view n)
      : kind(k), name(n) {}

  constexpr RuntimeUnitExport(RuntimeUnitExportKind k, std::string_view n,
                              int params, bool fn, std::string_view ret)
      : kind(k),
        name(n),
        param_count(params),
        is_function(fn),
        return_type_name(ret) {}

  RuntimeUnitExportKind kind;
  std::string_view name;
  int param_count = -1;
  bool is_function = true;
  std::string_view return_type_name;
};

struct RuntimeUnitModel {
  std::string_view name;
  std::span<const RuntimeUnitExport> exports;
};

const RuntimeUnitModel* runtime_unit_model(std::string_view name);
bool has_runtime_unit_model(std::string_view name);

}  // namespace tp2cc
