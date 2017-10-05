#include <algorithm>

#include "gtest/gtest.h"

#include "database/dbms.hpp"
#include "query/frontend/semantic/symbol_generator.hpp"
#include "query/frontend/semantic/symbol_table.hpp"
#include "query/plan/planner.hpp"
#include "utils/algorithm.hpp"

#include "query_plan_common.hpp"

using namespace query::plan;
using query::AstTreeStorage;
using Direction = query::EdgeAtom::Direction;

namespace std {

// Overloads for printing resulting rows from a query.
std::ostream &operator<<(std::ostream &stream,
                         const std::vector<TypedValue> &row) {
  utils::PrintIterable(stream, row);
  return stream;
}
std::ostream &operator<<(std::ostream &stream,
                         const std::vector<std::vector<TypedValue>> &rows) {
  utils::PrintIterable(stream, rows, "\n");
  return stream;
}

}  // namespace std

namespace {

auto MakeSymbolTable(query::Query &query) {
  query::SymbolTable symbol_table;
  query::SymbolGenerator symbol_generator(symbol_table);
  query.Accept(symbol_generator);
  return symbol_table;
}

void AssertRows(const std::vector<std::vector<TypedValue>> &datum,
                std::vector<std::vector<TypedValue>> expected) {
  auto row_equal = [](const auto &row1, const auto &row2) {
    if (row1.size() != row2.size()) {
      return false;
    }
    TypedValue::BoolEqual value_eq;
    auto row1_it = row1.begin();
    for (auto row2_it = row2.begin(); row2_it != row2.end();
         ++row1_it, ++row2_it) {
      if (!value_eq(*row1_it, *row2_it)) {
        return false;
      }
    }
    return true;
  };
  ASSERT_TRUE(std::is_permutation(datum.begin(), datum.end(), expected.begin(),
                                  expected.end(), row_equal))
      << "Actual rows:" << std::endl
      << datum << std::endl
      << "Expected rows:" << std::endl
      << expected;
};

void CheckPlansProduce(
    size_t expected_plan_count, AstTreeStorage &storage, GraphDbAccessor &dba,
    std::function<void(const std::vector<std::vector<TypedValue>> &)> check) {
  auto symbol_table = MakeSymbolTable(*storage.query());
  auto plans =
      MakeLogicalPlan<VariableStartPlanner>(storage, symbol_table, dba);
  EXPECT_EQ(std::distance(plans.begin(), plans.end()), expected_plan_count);
  for (const auto &plan : plans) {
    auto *produce = dynamic_cast<Produce *>(plan.get());
    ASSERT_TRUE(produce);
    auto results = CollectProduce(produce, symbol_table, dba);
    check(results);
  }
}

TEST(TestVariableStartPlanner, MatchReturn) {
  Dbms dbms;
  auto dba = dbms.active();
  // Make a graph (v1) -[:r]-> (v2)
  auto v1 = dba->InsertVertex();
  auto v2 = dba->InsertVertex();
  dba->InsertEdge(v1, v2, dba->EdgeType("r"));
  dba->AdvanceCommand();
  // Test MATCH (n) -[r]-> (m) RETURN n
  AstTreeStorage storage;
  QUERY(MATCH(PATTERN(NODE("n"), EDGE("r", Direction::OUT), NODE("m"))),
        RETURN("n"));
  // We have 2 nodes `n` and `m` from which we could start, so expect 2 plans.
  CheckPlansProduce(2, storage, *dba, [&](const auto &results) {
    // We expect to produce only a single (v1) node.
    AssertRows(results, {{v1}});
  });
}

TEST(TestVariableStartPlanner, MatchTripletPatternReturn) {
  Dbms dbms;
  auto dba = dbms.active();
  // Make a graph (v1) -[:r]-> (v2) -[:r]-> (v3)
  auto v1 = dba->InsertVertex();
  auto v2 = dba->InsertVertex();
  auto v3 = dba->InsertVertex();
  dba->InsertEdge(v1, v2, dba->EdgeType("r"));
  dba->InsertEdge(v2, v3, dba->EdgeType("r"));
  dba->AdvanceCommand();
  {
    // Test `MATCH (n) -[r]-> (m) -[e]-> (l) RETURN n`
    AstTreeStorage storage;
    QUERY(MATCH(PATTERN(NODE("n"), EDGE("r", Direction::OUT), NODE("m"),
                        EDGE("e", Direction::OUT), NODE("l"))),
          RETURN("n"));
    // We have 3 nodes: `n`, `m` and `l` from which we could start.
    CheckPlansProduce(3, storage, *dba, [&](const auto &results) {
      // We expect to produce only a single (v1) node.
      AssertRows(results, {{v1}});
    });
  }
  {
    // Equivalent to `MATCH (n) -[r]-> (m), (m) -[e]-> (l) RETURN n`.
    AstTreeStorage storage;
    QUERY(MATCH(PATTERN(NODE("n"), EDGE("r", Direction::OUT), NODE("m")),
                PATTERN(NODE("m"), EDGE("e", Direction::OUT), NODE("l"))),
          RETURN("n"));
    CheckPlansProduce(3, storage, *dba, [&](const auto &results) {
      AssertRows(results, {{v1}});
    });
  }
}

TEST(TestVariableStartPlanner, MatchOptionalMatchReturn) {
  Dbms dbms;
  auto dba = dbms.active();
  // Make a graph (v1) -[:r]-> (v2) -[:r]-> (v3)
  auto v1 = dba->InsertVertex();
  auto v2 = dba->InsertVertex();
  auto v3 = dba->InsertVertex();
  dba->InsertEdge(v1, v2, dba->EdgeType("r"));
  dba->InsertEdge(v2, v3, dba->EdgeType("r"));
  dba->AdvanceCommand();
  // Test MATCH (n) -[r]-> (m) OPTIONAL MATCH (m) -[e]-> (l) RETURN n, l
  AstTreeStorage storage;
  QUERY(
      MATCH(PATTERN(NODE("n"), EDGE("r", Direction::OUT), NODE("m"))),
      OPTIONAL_MATCH(PATTERN(NODE("m"), EDGE("e", Direction::OUT), NODE("l"))),
      RETURN("n", "l"));
  // We have 2 nodes `n` and `m` from which we could start the MATCH, and 2
  // nodes for OPTIONAL MATCH. This should produce 2 * 2 plans.
  CheckPlansProduce(4, storage, *dba, [&](const auto &results) {
    // We expect to produce 2 rows:
    //   * (v1), (v3)
    //   * (v2), null
    AssertRows(results, {{v1, v3}, {v2, TypedValue::Null}});
  });
}

TEST(TestVariableStartPlanner, MatchOptionalMatchMergeReturn) {
  Dbms dbms;
  auto dba = dbms.active();
  // Graph (v1) -[:r]-> (v2)
  auto v1 = dba->InsertVertex();
  auto v2 = dba->InsertVertex();
  auto r_type = dba->EdgeType("r");
  dba->InsertEdge(v1, v2, r_type);
  dba->AdvanceCommand();
  // Test MATCH (n) -[r]-> (m) OPTIONAL MATCH (m) -[e]-> (l)
  //      MERGE (u) -[q:r]-> (v) RETURN n, m, l, u, v
  AstTreeStorage storage;
  QUERY(
      MATCH(PATTERN(NODE("n"), EDGE("r", Direction::OUT), NODE("m"))),
      OPTIONAL_MATCH(PATTERN(NODE("m"), EDGE("e", Direction::OUT), NODE("l"))),
      MERGE(PATTERN(NODE("u"), EDGE("q", Direction::OUT, {r_type}), NODE("v"))),
      RETURN("n", "m", "l", "u", "v"));
  // Since MATCH, OPTIONAL MATCH and MERGE each have 2 nodes from which we can
  // start, we generate 2 * 2 * 2 plans.
  CheckPlansProduce(8, storage, *dba, [&](const auto &results) {
    // We expect to produce a single row: (v1), (v2), null, (v1), (v2)
    AssertRows(results, {{v1, v2, TypedValue::Null, v1, v2}});
  });
}

TEST(TestVariableStartPlanner, MatchWithMatchReturn) {
  Dbms dbms;
  auto dba = dbms.active();
  // Graph (v1) -[:r]-> (v2)
  auto v1 = dba->InsertVertex();
  auto v2 = dba->InsertVertex();
  dba->InsertEdge(v1, v2, dba->EdgeType("r"));
  dba->AdvanceCommand();
  // Test MATCH (n) -[r]-> (m) WITH n MATCH (m) -[r]-> (l) RETURN n, m, l
  AstTreeStorage storage;
  QUERY(MATCH(PATTERN(NODE("n"), EDGE("r", Direction::OUT), NODE("m"))),
        WITH("n"),
        MATCH(PATTERN(NODE("m"), EDGE("r", Direction::OUT), NODE("l"))),
        RETURN("n", "m", "l"));
  // We can start from 2 nodes in each match. Since WITH separates query parts,
  // we expect to get 2 plans for each, which totals 2 * 2.
  CheckPlansProduce(4, storage, *dba, [&](const auto &results) {
    // We expect to produce a single row: (v1), (v1), (v2)
    AssertRows(results, {{v1, v1, v2}});
  });
}

TEST(TestVariableStartPlanner, MatchVariableExpand) {
  Dbms dbms;
  auto dba = dbms.active();
  // Graph (v1) -[:r1]-> (v2) -[:r2]-> (v3)
  auto v1 = dba->InsertVertex();
  auto v2 = dba->InsertVertex();
  auto v3 = dba->InsertVertex();
  auto r1 = dba->InsertEdge(v1, v2, dba->EdgeType("r1"));
  auto r2 = dba->InsertEdge(v2, v3, dba->EdgeType("r2"));
  dba->AdvanceCommand();
  // Test MATCH (n) -[r*]-> (m) RETURN r
  AstTreeStorage storage;
  auto edge = EDGE_VARIABLE("r", Direction::OUT);
  QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("r"));
  // We expect to get a single column with the following rows:
  TypedValue r1_list(std::vector<TypedValue>{r1});         // [r1]
  TypedValue r2_list(std::vector<TypedValue>{r2});         // [r2]
  TypedValue r1_r2_list(std::vector<TypedValue>{r1, r2});  // [r1, r2]
  CheckPlansProduce(2, storage, *dba, [&](const auto &results) {
    AssertRows(results, {{r1_list}, {r2_list}, {r1_r2_list}});
  });
}

TEST(TestVariableStartPlanner, MatchVariableExpandReferenceNode) {
  Dbms dbms;
  auto dba = dbms.active();
  auto id = dba->Property("id");
  // Graph (v1 {id:1}) -[:r1]-> (v2 {id: 2}) -[:r2]-> (v3 {id: 3})
  auto v1 = dba->InsertVertex();
  v1.PropsSet(id, 1);
  auto v2 = dba->InsertVertex();
  v2.PropsSet(id, 2);
  auto v3 = dba->InsertVertex();
  v3.PropsSet(id, 3);
  auto r1 = dba->InsertEdge(v1, v2, dba->EdgeType("r1"));
  auto r2 = dba->InsertEdge(v2, v3, dba->EdgeType("r2"));
  dba->AdvanceCommand();
  // Test MATCH (n) -[r*..n.id]-> (m) RETURN r
  AstTreeStorage storage;
  auto edge = EDGE_VARIABLE("r", Direction::OUT);
  edge->upper_bound_ = PROPERTY_LOOKUP("n", id);
  QUERY(MATCH(PATTERN(NODE("n"), edge, NODE("m"))), RETURN("r"));
  // We expect to get a single column with the following rows:
  TypedValue r1_list(std::vector<TypedValue>{r1});  // [r1] (v1 -[*..1]-> v2)
  TypedValue r2_list(std::vector<TypedValue>{r2});  // [r2] (v2 -[*..2]-> v3)
  CheckPlansProduce(2, storage, *dba, [&](const auto &results) {
    AssertRows(results, {{r1_list}, {r2_list}});
  });
}

TEST(TestVariableStartPlanner, MatchVariableExpandBoth) {
  Dbms dbms;
  auto dba = dbms.active();
  auto id = dba->Property("id");
  // Graph (v1 {id:1}) -[:r1]-> (v2) -[:r2]-> (v3)
  auto v1 = dba->InsertVertex();
  v1.PropsSet(id, 1);
  auto v2 = dba->InsertVertex();
  auto v3 = dba->InsertVertex();
  auto r1 = dba->InsertEdge(v1, v2, dba->EdgeType("r1"));
  auto r2 = dba->InsertEdge(v2, v3, dba->EdgeType("r2"));
  dba->AdvanceCommand();
  // Test MATCH (n {id:1}) -[r*]- (m) RETURN r
  AstTreeStorage storage;
  auto edge = EDGE_VARIABLE("r", Direction::BOTH);
  auto node_n = NODE("n");
  node_n->properties_[std::make_pair("id", id)] = LITERAL(1);
  QUERY(MATCH(PATTERN(node_n, edge, NODE("m"))), RETURN("r"));
  // We expect to get a single column with the following rows:
  TypedValue r1_list(std::vector<TypedValue>{r1});         // [r1]
  TypedValue r1_r2_list(std::vector<TypedValue>{r1, r2});  // [r1, r2]
  CheckPlansProduce(2, storage, *dba, [&](const auto &results) {
    AssertRows(results, {{r1_list}, {r1_r2_list}});
  });
}

TEST(TestVariableStartPlanner, MatchBfs) {
  Dbms dbms;
  auto dba = dbms.active();
  auto id = dba->Property("id");
  // Graph (v1 {id:1}) -[:r1]-> (v2 {id: 2}) -[:r2]-> (v3 {id: 3})
  auto v1 = dba->InsertVertex();
  v1.PropsSet(id, 1);
  auto v2 = dba->InsertVertex();
  v2.PropsSet(id, 2);
  auto v3 = dba->InsertVertex();
  v3.PropsSet(id, 3);
  auto r1 = dba->InsertEdge(v1, v2, dba->EdgeType("r1"));
  dba->InsertEdge(v2, v3, dba->EdgeType("r2"));
  dba->AdvanceCommand();
  // Test MATCH (n) -[r *bfs..10](r, n | n.id <> 3)]-> (m) RETURN r
  AstTreeStorage storage;
  auto *bfs = storage.Create<query::EdgeAtom>(
      IDENT("r"), EdgeAtom::Type::BREADTH_FIRST, Direction::OUT,
      std::vector<GraphDbTypes::EdgeType>{});
  bfs->inner_edge_ = IDENT("r");
  bfs->inner_node_ = IDENT("n");
  bfs->filter_expression_ = NEQ(PROPERTY_LOOKUP("n", id), LITERAL(3));
  bfs->upper_bound_ = LITERAL(10);
  QUERY(MATCH(PATTERN(NODE("n"), bfs, NODE("m"))), RETURN("r"));
  // We expect to get a single column with the following rows:
  TypedValue r1_list(std::vector<TypedValue>{r1});  // [r1]
  CheckPlansProduce(2, storage, *dba, [&](const auto &results) {
    AssertRows(results, {{r1_list}});
  });
}

}  // namespace
