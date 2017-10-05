// Copyright 2017 Memgraph
//
// Created by Teon Banek on 11-03-2017

#pragma once

#include "query/exceptions.hpp"
#include "query/frontend/ast/ast.hpp"
#include "query/frontend/semantic/symbol_table.hpp"

namespace query {

///
/// Visits the AST and generates symbols for variables.
///
/// During the process of symbol generation, simple semantic checks are
/// performed. Such as, redeclaring a variable or conflicting expectations of
/// variable types.
class SymbolGenerator : public HierarchicalTreeVisitor {
 public:
  SymbolGenerator(SymbolTable &symbol_table) : symbol_table_(symbol_table) {}

  using HierarchicalTreeVisitor::PreVisit;
  using typename HierarchicalTreeVisitor::ReturnType;
  using HierarchicalTreeVisitor::Visit;
  using HierarchicalTreeVisitor::PostVisit;

  // Clauses
  bool PreVisit(Create &) override;
  bool PostVisit(Create &) override;
  bool PreVisit(Return &) override;
  bool PreVisit(With &) override;
  bool PreVisit(Where &) override;
  bool PostVisit(Where &) override;
  bool PreVisit(Merge &) override;
  bool PostVisit(Merge &) override;
  bool PostVisit(Unwind &) override;
  bool PreVisit(Match &) override;
  bool PostVisit(Match &) override;
  bool Visit(CreateIndex &) override;

  // Expressions
  ReturnType Visit(Identifier &) override;
  ReturnType Visit(PrimitiveLiteral &) override { return true; }
  ReturnType Visit(ParameterLookup &) override { return true; }
  bool PreVisit(Aggregation &) override;
  bool PostVisit(Aggregation &) override;
  bool PreVisit(IfOperator &) override;
  bool PostVisit(IfOperator &) override;
  bool PreVisit(All &) override;

  // Pattern and its subparts.
  bool PreVisit(Pattern &) override;
  bool PostVisit(Pattern &) override;
  bool PreVisit(NodeAtom &) override;
  bool PostVisit(NodeAtom &) override;
  bool PreVisit(EdgeAtom &) override;
  bool PostVisit(EdgeAtom &) override;

 private:
  // Scope stores the state of where we are when visiting the AST and a map of
  // names to symbols.
  struct Scope {
    bool in_pattern{false};
    bool in_merge{false};
    bool in_create{false};
    // in_create_node is true if we are creating or merging *only* a node.
    // Therefore, it is *not* equivalent to (in_create || in_merge) &&
    // in_node_atom.
    bool in_create_node{false};
    // True if creating an edge;
    // shortcut for (in_create || in_merge) && visiting_edge.
    bool in_create_edge{false};
    bool in_node_atom{false};
    EdgeAtom *visiting_edge{nullptr};
    bool in_aggregation{false};
    bool in_return{false};
    bool in_with{false};
    bool in_skip{false};
    bool in_limit{false};
    bool in_order_by{false};
    bool in_where{false};
    bool in_match{false};
    // True when visiting a pattern atom (node or edge) identifier, which can be
    // reused or created in the pattern itself.
    bool in_pattern_atom_identifier{false};
    // True when visiting range bounds of a variable path.
    bool in_edge_range{false};
    // True if the return/with contains an aggregation in any named expression.
    bool has_aggregation{false};
    // Map from variable names to symbols.
    std::map<std::string, Symbol> symbols;
    // Identifiers found in property maps of patterns or as variable length path
    // bounds in a single Match clause. They need to be checked after visiting
    // Match. Identifiers created by naming vertices, edges and paths are *not*
    // stored in here.
    std::vector<Identifier *> identifiers_in_match;
    // Number of nested IfOperators.
    int num_if_operators{0};
  };

  bool HasSymbol(const std::string &name);

  // Returns a freshly generated symbol. Previous mapping of the same name to a
  // different symbol is replaced with the new one.
  auto CreateSymbol(const std::string &name, bool user_declared,
                    Symbol::Type type = Symbol::Type::Any,
                    int token_position = -1);

  // Returns the symbol by name. If the mapping already exists, checks if the
  // types match. Otherwise, returns a new symbol.
  auto GetOrCreateSymbol(const std::string &name, bool user_declared,
                         Symbol::Type type = Symbol::Type::Any);

  void VisitReturnBody(ReturnBody &body, Where *where = nullptr);

  void VisitWithIdentifiers(Tree &, const std::vector<Identifier *> &);

  SymbolTable &symbol_table_;
  Scope scope_;
};

}  // namespace query
