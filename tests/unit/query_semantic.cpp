#include <memory>

#include "gtest/gtest.h"

#include "query/frontend/ast/ast.hpp"
#include "query/frontend/interpret/interpret.hpp"
#include "query/frontend/semantic/symbol_table.hpp"
#include "query/frontend/semantic/symbol_generator.hpp"

#include "query_common.hpp"

using namespace query;

namespace {

TEST(TestSymbolGenerator, MatchNodeReturn) {
  SymbolTable symbol_table;
  AstTreeStorage storage;
  // MATCH (node_atom_1) RETURN node_atom_1 AS node_atom_1
  auto query_ast = QUERY(MATCH(PATTERN(NODE("node_atom_1"))),
                         RETURN(NEXPR("node_atom_1", IDENT("node_atom_1"))));
  SymbolGenerator symbol_generator(symbol_table);
  query_ast->Accept(symbol_generator);
  EXPECT_EQ(symbol_table.max_position(), 2);
  auto match = dynamic_cast<Match *>(query_ast->clauses_[0]);
  auto pattern = match->patterns_[0];
  auto node_atom = dynamic_cast<NodeAtom *>(pattern->atoms_[0]);
  auto node_sym = symbol_table[*node_atom->identifier_];
  EXPECT_EQ(node_sym.name_, "node_atom_1");
  auto ret = dynamic_cast<Return *>(query_ast->clauses_[1]);
  auto named_expr = ret->named_expressions_[0];
  auto column_sym = symbol_table[*named_expr];
  EXPECT_EQ(node_sym.name_, column_sym.name_);
  EXPECT_NE(node_sym, column_sym);
  auto ret_sym = symbol_table[*named_expr->expression_];
  EXPECT_EQ(node_sym, ret_sym);
}

TEST(TestSymbolGenerator, MatchUnboundMultiReturn) {
  SymbolTable symbol_table;
  AstTreeStorage storage;
  // AST using variable in return bound by naming the previous return
  // expression. This is treated as an unbound variable.
  // MATCH (node_atom_1) RETURN node_atom_1 AS n, n AS n
  auto query_ast =
      QUERY(MATCH(PATTERN(NODE("node_atom_1"))),
            RETURN(NEXPR("n", IDENT("node_atom_1")), NEXPR("n", IDENT("n"))));
  SymbolGenerator symbol_generator(symbol_table);
  EXPECT_THROW(query_ast->Accept(symbol_generator), UnboundVariableError);
}

TEST(TestSymbolGenerator, MatchNodeUnboundReturn) {
  SymbolTable symbol_table;
  AstTreeStorage storage;
  // AST with unbound variable in return: MATCH (n) RETURN x AS x
  auto query_ast =
      QUERY(MATCH(PATTERN(NODE("n"))), RETURN(NEXPR("x", IDENT("x"))));
  SymbolGenerator symbol_generator(symbol_table);
  EXPECT_THROW(query_ast->Accept(symbol_generator), UnboundVariableError);
}

TEST(TestSymbolGenerator, MatchSameEdge) {
  SymbolTable symbol_table;
  AstTreeStorage storage;
  // AST with match pattern referencing an edge multiple times:
  // MATCH (n) -[r]- (n) -[r]- (n) RETURN r AS r
  // This usually throws a redeclaration error, but we support it.
  auto query_ast = QUERY(
      MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("n"), EDGE("r"), NODE("n"))),
      RETURN(NEXPR("r", IDENT("r"))));
  SymbolGenerator symbol_generator(symbol_table);
  query_ast->Accept(symbol_generator);
  EXPECT_EQ(symbol_table.max_position(), 3);
  auto match = dynamic_cast<Match *>(query_ast->clauses_[0]);
  auto pattern = match->patterns_[0];
  std::vector<Symbol> node_symbols;
  std::vector<Symbol> edge_symbols;
  bool is_node{true};
  for (auto &atom : pattern->atoms_) {
    auto symbol = symbol_table[*atom->identifier_];
    if (is_node) {
      node_symbols.emplace_back(symbol);
    } else {
      edge_symbols.emplace_back(symbol);
    }
    is_node = !is_node;
  }
  auto &node_symbol = node_symbols.front();
  for (auto &symbol : node_symbols) {
    EXPECT_EQ(node_symbol, symbol);
  }
  auto &edge_symbol = edge_symbols.front();
  for (auto &symbol : edge_symbols) {
    EXPECT_EQ(edge_symbol, symbol);
  }
  auto ret = dynamic_cast<Return *>(query_ast->clauses_[1]);
  auto named_expr = ret->named_expressions_[0];
  auto ret_symbol = symbol_table[*named_expr->expression_];
  EXPECT_EQ(edge_symbol, ret_symbol);
}

TEST(TestSymbolGenerator, CreatePropertyUnbound) {
  SymbolTable symbol_table;
  AstTreeStorage storage;
  // AST with unbound variable in create: CREATE ({prop: x})
  auto node = NODE("anon");
  std::string prop_name = "prop";
  node->properties_[&prop_name] = IDENT("x");
  auto query_ast = QUERY(CREATE(PATTERN(node)));
  SymbolGenerator symbol_generator(symbol_table);
  EXPECT_THROW(query_ast->Accept(symbol_generator), UnboundVariableError);
}

TEST(TestSymbolGenerator, CreateNodeReturn) {
  SymbolTable symbol_table;
  AstTreeStorage storage;
  // Simple AST returning a created node: CREATE (n) RETURN n
  auto query_ast =
      QUERY(CREATE(PATTERN(NODE("n"))), RETURN(NEXPR("n", IDENT("n"))));
  SymbolGenerator symbol_generator(symbol_table);
  query_ast->Accept(symbol_generator);
  EXPECT_EQ(symbol_table.max_position(), 2);
  auto create = dynamic_cast<Create *>(query_ast->clauses_[0]);
  auto pattern = create->patterns_[0];
  auto node_atom = dynamic_cast<NodeAtom *>(pattern->atoms_[0]);
  auto node_sym = symbol_table[*node_atom->identifier_];
  EXPECT_EQ(node_sym.name_, "n");
  auto ret = dynamic_cast<Return *>(query_ast->clauses_[1]);
  auto named_expr = ret->named_expressions_[0];
  auto column_sym = symbol_table[*named_expr];
  EXPECT_EQ(node_sym.name_, column_sym.name_);
  EXPECT_NE(node_sym, column_sym);
  auto ret_sym = symbol_table[*named_expr->expression_];
  EXPECT_EQ(node_sym, ret_sym);
}

TEST(TestSymbolGenerator, CreateRedeclareNode) {
  SymbolTable symbol_table;
  AstTreeStorage storage;
  // AST with redeclaring a variable when creating nodes: CREATE (n), (n)
  auto query_ast = QUERY(CREATE(PATTERN(NODE("n")), PATTERN(NODE("n"))));
  SymbolGenerator symbol_generator(symbol_table);
  EXPECT_THROW(query_ast->Accept(symbol_generator), RedeclareVariableError);
}

TEST(TestSymbolGenerator, MultiCreateRedeclareNode) {
  SymbolTable symbol_table;
  AstTreeStorage storage;
  // AST with redeclaring a variable when creating nodes with multiple creates:
  // CREATE (n) CREATE (n)
  auto query_ast =
      QUERY(CREATE(PATTERN(NODE("n"))), CREATE(PATTERN(NODE("n"))));
  SymbolGenerator symbol_generator(symbol_table);
  EXPECT_THROW(query_ast->Accept(symbol_generator), RedeclareVariableError);
}

TEST(TestSymbolGenerator, MatchCreateRedeclareNode) {
  SymbolTable symbol_table;
  AstTreeStorage storage;
  // AST with redeclaring a match node variable in create: MATCH (n) CREATE (n)
  auto query_ast = QUERY(MATCH(PATTERN(NODE("n"))), CREATE(PATTERN(NODE("n"))));
  SymbolGenerator symbol_generator(symbol_table);
  EXPECT_THROW(query_ast->Accept(symbol_generator), RedeclareVariableError);
}

TEST(TestSymbolGenerator, MatchCreateRedeclareEdge) {
  SymbolTable symbol_table;
  AstTreeStorage storage;
  // AST with redeclaring a match edge variable in create:
  // MATCH (n) -[r]- (m) CREATE (n) -[r :relationship]-> (l)
  std::string relationship("relationship");
  auto query = QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("m"))),
                     CREATE(PATTERN(NODE("n"), EDGE("r", &relationship,
                                                    EdgeAtom::Direction::RIGHT),
                                    NODE("l"))));
  SymbolGenerator symbol_generator(symbol_table);
  EXPECT_THROW(query->Accept(symbol_generator), RedeclareVariableError);
}

TEST(TestSymbolGenerator, MatchTypeMismatch) {
  AstTreeStorage storage;
  // Using an edge variable as a node causes a type mismatch.
  // MATCH (n) -[r]-> (r)
  auto query = QUERY(MATCH(PATTERN(NODE("n"), EDGE("r"), NODE("r"))));
  SymbolTable symbol_table;
  SymbolGenerator symbol_generator(symbol_table);
  EXPECT_THROW(query->Accept(symbol_generator), TypeMismatchError);
}

TEST(TestSymbolGenerator, MatchCreateTypeMismatch) {
  AstTreeStorage storage;
  // Using an edge variable as a node causes a type mismatch.
  // MATCH (n1) -[r1]- (n2) CREATE (r1) -[r2]-> (n2)
  auto query =
      QUERY(MATCH(PATTERN(NODE("n1"), EDGE("r1"), NODE("n2"))),
            CREATE(PATTERN(NODE("r1"), EDGE("r2", EdgeAtom::Direction::RIGHT),
                           NODE("n2"))));
  SymbolTable symbol_table;
  SymbolGenerator symbol_generator(symbol_table);
  EXPECT_THROW(query->Accept(symbol_generator), TypeMismatchError);
}

TEST(TestSymbolGenerator, CreateMultipleEdgeType) {
  AstTreeStorage storage;
  // Multiple edge relationship are not allowed when creating edges.
  // CREATE (n) -[r :rel1 | :rel2]-> (m)
  std::string rel1("rel1");
  std::string rel2("rel2");
  auto edge = EDGE("r", &rel1, EdgeAtom::Direction::RIGHT);
  edge->edge_types_.emplace_back(&rel2);
  auto query = QUERY(CREATE(PATTERN(NODE("n"), edge, NODE("m"))));
  SymbolTable symbol_table;
  SymbolGenerator symbol_generator(symbol_table);
  EXPECT_THROW(query->Accept(symbol_generator), SemanticException);
}

TEST(TestSymbolGenerator, CreateBidirectionalEdge) {
  AstTreeStorage storage;
  // Bidirectional relationships are not allowed when creating edges.
  // CREATE (n) -[r :rel1]- (m)
  std::string rel1("rel1");
  auto query = QUERY(CREATE(PATTERN(NODE("n"), EDGE("r", &rel1), NODE("m"))));
  SymbolTable symbol_table;
  SymbolGenerator symbol_generator(symbol_table);
  EXPECT_THROW(query->Accept(symbol_generator), SemanticException);
}

TEST(TestSymbolGenerator, MatchWhereUnbound) {
  // Test MATCH (n) WHERE missing < 42 RETURN n AS n
  AstTreeStorage storage;
  std::string property("property");
  auto match = MATCH(PATTERN(NODE("n")));
  match->where_ = WHERE(LESS(IDENT("missing"), LITERAL(42)));
  auto query = QUERY(match, RETURN(NEXPR("n", IDENT("n"))));
  SymbolTable symbol_table;
  SymbolGenerator symbol_generator(symbol_table);
  EXPECT_THROW(query->Accept(symbol_generator), UnboundVariableError);
}

}
