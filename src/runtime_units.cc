#include "runtime_units.h"

#include <string>

#include "emit_support.h"

namespace tp2cc {

namespace {

constexpr RuntimeUnitExport kDosExports[] = {
    {RuntimeUnitExportKind::TypeAlias, "DateTime"},
    {RuntimeUnitExportKind::TypeAlias, "DirStr"},
    {RuntimeUnitExportKind::TypeAlias, "NameStr"},
    {RuntimeUnitExportKind::TypeAlias, "ExtStr"},
    {RuntimeUnitExportKind::TypeAlias, "PathStr"},
    {RuntimeUnitExportKind::TypeAlias, "SearchRec"},
    {RuntimeUnitExportKind::TypeAlias, "TSearchRec"},
    {RuntimeUnitExportKind::Const, "ReadOnly"},
    {RuntimeUnitExportKind::Const, "Hidden"},
    {RuntimeUnitExportKind::Const, "SysFile"},
    {RuntimeUnitExportKind::Const, "VolumeID"},
    {RuntimeUnitExportKind::Const, "Directory"},
    {RuntimeUnitExportKind::Const, "Archive"},
    {RuntimeUnitExportKind::Const, "AnyFile"},
    {RuntimeUnitExportKind::Var, "DosError"},
    {RuntimeUnitExportKind::Proc, "ChDir"},
    {RuntimeUnitExportKind::Proc, "DiskSize"},
    {RuntimeUnitExportKind::Proc, "DosExitCode"},
    {RuntimeUnitExportKind::Proc, "Exec"},
    {RuntimeUnitExportKind::Proc, "EpochToLocal"},
    {RuntimeUnitExportKind::Proc, "FExpand"},
    {RuntimeUnitExportKind::Proc, "FindClose"},
    {RuntimeUnitExportKind::Proc, "FindFirst"},
    {RuntimeUnitExportKind::Proc, "FindNext"},
    {RuntimeUnitExportKind::Proc, "FSearch"},
    {RuntimeUnitExportKind::Proc, "FSplit"},
    {RuntimeUnitExportKind::Proc, "GetDir"},
    {RuntimeUnitExportKind::Proc, "GetDate"},
    {RuntimeUnitExportKind::Proc, "GetEnv"},
    {RuntimeUnitExportKind::Proc, "GetFAttr"},
    {RuntimeUnitExportKind::Proc, "GetFTime"},
    {RuntimeUnitExportKind::Proc, "GetTime"},
    {RuntimeUnitExportKind::Proc, "MkDir"},
    {RuntimeUnitExportKind::Proc, "PackTime"},
    {RuntimeUnitExportKind::Proc, "RmDir"},
    {RuntimeUnitExportKind::Proc, "SetFTime"},
    {RuntimeUnitExportKind::Proc, "UnpackTime"},
};

constexpr RuntimeUnitExport kLinuxExports[] = {
    {RuntimeUnitExportKind::Proc, "GetEnv", 1, true, "pchar"},
    {RuntimeUnitExportKind::Proc, "Shell", 1, true, "longint"},
};

constexpr RuntimeUnitExport kUnixExports[] = {
    {RuntimeUnitExportKind::Proc, "GetEnv", 1, true, "pchar"},
    {RuntimeUnitExportKind::Proc, "Shell", 1, true, "longint"},
    {RuntimeUnitExportKind::Proc, "FpSystem", 1, true, "longint"},
};

constexpr RuntimeUnitExport kBaseUnixExports[] = {
    {RuntimeUnitExportKind::Proc, "FpGetEnv", 1, true, "pchar"},
    {RuntimeUnitExportKind::Proc, "FpChmod", 2, true, "longint"},
};

constexpr RuntimeUnitExport kStringsExports[] = {
    {RuntimeUnitExportKind::Proc, "StrLen"},
    {RuntimeUnitExportKind::Proc, "StrPCopy"},
    {RuntimeUnitExportKind::Proc, "StrPas"},
    {RuntimeUnitExportKind::Proc, "StrComp"},
    {RuntimeUnitExportKind::Proc, "StrRScan"},
    {RuntimeUnitExportKind::Proc, "StrNew"},
    {RuntimeUnitExportKind::Proc, "StrDispose"},
};

constexpr RuntimeUnitExport kSysutilsExports[] = {
    {RuntimeUnitExportKind::TypeAlias, "HRESULT"},
    {RuntimeUnitExportKind::TypeAlias, "PAnsiString"},
    {RuntimeUnitExportKind::TypeAlias, "PDWord"},
    {RuntimeUnitExportKind::TypeAlias, "PLongWord"},
    {RuntimeUnitExportKind::TypeAlias, "PQWord"},
    {RuntimeUnitExportKind::TypeAlias, "PShortString"},
    {RuntimeUnitExportKind::TypeAlias, "TDateTime"},
    {RuntimeUnitExportKind::TypeAlias, "TExecuteFlags"},
    {RuntimeUnitExportKind::TypeAlias, "TSysCharSet"},
    {RuntimeUnitExportKind::TypeAlias, "TSystemTime"},
    {RuntimeUnitExportKind::RuntimeClass, "Exception"},
    {RuntimeUnitExportKind::RuntimeClass, "EExternal"},
    {RuntimeUnitExportKind::RuntimeClass, "EIntError"},
    {RuntimeUnitExportKind::RuntimeClass, "EInOutError"},
    {RuntimeUnitExportKind::RuntimeClass, "EHeapMemoryError"},
    {RuntimeUnitExportKind::RuntimeClass, "EHeapException"},
    {RuntimeUnitExportKind::RuntimeClass, "EOutOfMemory"},
    {RuntimeUnitExportKind::RuntimeClass, "EIntOverflow"},
    {RuntimeUnitExportKind::RuntimeClass, "ERangeError"},
    {RuntimeUnitExportKind::RuntimeClass, "EDivByZero"},
    {RuntimeUnitExportKind::RuntimeClass, "EOSError"},
    {RuntimeUnitExportKind::Proc, "AnsiCompareFileName"},
    {RuntimeUnitExportKind::Proc, "ChangeFileExt"},
    {RuntimeUnitExportKind::Proc, "CompareText"},
    {RuntimeUnitExportKind::Proc, "Date"},
    {RuntimeUnitExportKind::Proc, "DecodeDate"},
    {RuntimeUnitExportKind::Proc, "DecodeTime"},
    {RuntimeUnitExportKind::Proc, "DirectoryExists"},
    {RuntimeUnitExportKind::Proc, "ExecuteProcess"},
    {RuntimeUnitExportKind::Proc, "ExpandFileName"},
    {RuntimeUnitExportKind::Proc, "FileAge"},
    {RuntimeUnitExportKind::Proc, "FileDateToDateTime"},
    {RuntimeUnitExportKind::Proc, "FileExists"},
    {RuntimeUnitExportKind::Proc, "FileGetDate"},
    {RuntimeUnitExportKind::Proc, "FileSetDate"},
    {RuntimeUnitExportKind::Proc, "GetEnvironmentVariable"},
    {RuntimeUnitExportKind::Proc, "GetFileHandle"},
    {RuntimeUnitExportKind::Proc, "GetLocalTime"},
    {RuntimeUnitExportKind::Proc, "IntToStr"},
    {RuntimeUnitExportKind::Proc, "RenameFile"},
    {RuntimeUnitExportKind::Proc, "SetDirSeparators"},
    {RuntimeUnitExportKind::Proc, "StrPas"},
    {RuntimeUnitExportKind::Proc, "StringOfChar"},
    {RuntimeUnitExportKind::Proc, "Time"},
    {RuntimeUnitExportKind::Proc, "Trim"},
};

constexpr RuntimeUnitModel kModels[] = {
    {"baseunix", std::span<const RuntimeUnitExport>(kBaseUnixExports)},
    {"dos", std::span<const RuntimeUnitExport>(kDosExports)},
    {"linux", std::span<const RuntimeUnitExport>(kLinuxExports)},
    {"math", std::span<const RuntimeUnitExport>()},
    {"strings", std::span<const RuntimeUnitExport>(kStringsExports)},
    {"sysutils", std::span<const RuntimeUnitExport>(kSysutilsExports)},
    {"unix", std::span<const RuntimeUnitExport>(kUnixExports)},
};

}  // namespace

const RuntimeUnitModel* runtime_unit_model(std::string_view name) {
  const std::string low = ascii_lower(name);
  for (const auto& model : kModels) {
    if (low == model.name) return &model;
  }
  return nullptr;
}

bool has_runtime_unit_model(std::string_view name) {
  return runtime_unit_model(name) != nullptr;
}

}  // namespace tp2cc
