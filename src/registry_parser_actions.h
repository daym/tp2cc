#pragma once

#include <string>
#include <vector>

#include "parser.h"
#include "typereg.h"

namespace tp2cc {

// Parser semantic sink for a source whose imported interfaces have already
// been registered. UnitGraph supplies its own sink because it must recursively
// load those interfaces before forwarding the import event.
class RegistryParserActions final : public ParserSemanticActions {
 public:
  explicit RegistryParserActions(TypeRegistry& registry)
      : registry_(registry) {}

  void begin_compilation_unit(std::string_view name,
                              bool) override {
    unit_ = std::string(name);
    registry_.begin_parsed_unit(unit_);
  }

  void import_units(const std::vector<std::string>& units,
                    bool in_interface) override {
    registry_.set_parsed_unit_imports(unit_, units, in_interface);
  }

  void parsed_type_section(
      const std::vector<ast::DeclPtr>& declarations,
      bool in_interface) override {
    registry_.bind_parsed_declarations(unit_, declarations, in_interface);
  }

  void parsed_declaration(const ast::DeclPtr& declaration,
                          bool in_interface) override {
    registry_.bind_parsed_declarations(
        unit_, std::vector<ast::DeclPtr>{declaration}, in_interface);
  }

  void finish_compilation_unit(const ast::UnitNode& unit) override {
    registry_.bind_parsed_unit_bodies(unit);
  }

 private:
  TypeRegistry& registry_;
  std::string unit_;
};

}  // namespace tp2cc
