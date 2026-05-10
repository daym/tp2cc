#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ast.h"
#include "emit_context.h"

namespace tp2cc {

class EmitAnalysis;
class EmitStorage;
class EmitTypes;
class EmitValues;
struct MethodSig;
struct TypeRegistry;

class EmitDeclOps {
 public:
  virtual ~EmitDeclOps() = default;
  virtual void emitln(std::string_view s) = 0;
  virtual void nl() = 0;
  virtual void indent() = 0;
  virtual void dedent() = 0;
  virtual void report_error(Location where, const std::string& msg) = 0;
  virtual void emit_proc_body(const ast::ProcDecl& pd) = 0;
  virtual void emit_nested_proc_lambda(const ast::ProcDecl& pd) = 0;
  virtual bool in_block_scope() const = 0;
};

// Pascal declaration emission. This module owns how const/type/var/proc
// declarations spell as C++, including packed-record layout asserts, object
// method declarations, metaclass descriptor surfaces, and forward/prototype
// handling for nested and top-level routines.
class EmitDecls {
 public:
  EmitDecls(const TypeRegistry* registry, ScopeStateView& scope,
            EmitAnalysis& analysis,
            EmitTypes& types, EmitStorage& storage, EmitValues& values,
            EmitDeclOps& emit_ops);

  void emit_const_decl(const ast::ConstDecl& cd, bool in_header);
  void emit_type_decl(const ast::TypeDecl& td, bool in_header);
  void emit_var_decl(const ast::VarDecl& vd, bool in_header);
  std::string param_type_list_to_cxx(const std::vector<ast::Param>& params);
  std::string param_list_to_cxx(const std::vector<ast::Param>& params);
  std::string proc_attributes_to_cxx(const ast::ProcDecl& pd);
  std::string proc_return_type_to_cxx(const ast::ProcDecl& pd);
  void emit_proc_decl_signature(const ast::ProcDecl& pd);
  void emit_decl(const ast::Decl& d, bool in_header);

 private:
  struct MetaclassCallable {
    std::string name;
    const MethodSig* sig = nullptr;
    bool implicit_root_create = false;
  };

  struct MetaclassCallableImpl {
    std::string owner_class;
    const MethodSig* sig = nullptr;
    bool implicit_root_create = false;
  };

  void emit_packed_record_asserts(const std::string& type_text,
                                  const std::vector<std::pair<std::string,
                                                              std::string>>&
                                      field_offsets,
                                  std::string_view size_expr,
                                  std::string_view label);
  void emit_method_pointer_thunk(const std::string& owner_name,
                                 const ast::ProcDecl& pd,
                                 const std::string& ret);
  std::vector<MetaclassCallable> collect_metaclass_callables(
      std::string_view class_name);
  std::optional<MetaclassCallableImpl> find_metaclass_callable_impl(
      std::string_view concrete_class, const MetaclassCallable& target);
  void emit_enum_carrier(const ast::TyEnum& te, std::string_view cxx_name,
                         std::string_view bound_name);
  void emit_enum_carrier_decls(const ast::TypeExpr* t,
                               const ast::TyEnum* skip = nullptr);
  bool should_emit_var_type_helpers(const ast::VarDecl& vd, bool in_header);

  const TypeRegistry* registry_;
  ScopeStateView& scope_;
  EmitAnalysis& analysis_;
  EmitTypes& types_;
  EmitStorage& storage_;
  EmitValues& values_;
  EmitDeclOps& emit_ops_;
};

}  // namespace tp2cc
