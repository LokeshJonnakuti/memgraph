#include <iostream>
#include <string>

#include "query/parameters.hpp"
#include "query/plan_interface.hpp"
#include "query/typed_value.hpp"
#include "storage/edge_accessor.hpp"
#include "storage/vertex_accessor.hpp"
#include "using.hpp"

using std::cout;
using std::endl;
using query::TypedValue;

// Query: CREATE (g:garment {garment_id: 1234, garment_category_id:
// 1,reveals:30}) RETURN g

class CPUPlan : public PlanInterface<Stream> {
 public:
  bool run(GraphDbAccessor &db_accessor, const Parameters &args,
           Stream &stream) {
    auto v = db_accessor.InsertVertex();
    v.add_label(db_accessor.Label("garment"));
    v.PropsSet(db_accessor.Property("garment_id"), args.At(0).second);
    v.PropsSet(db_accessor.Property("garment_category_id"), args.At(1).second);
    v.PropsSet(db_accessor.Property("reveals"), args.At(2).second);
    std::vector<std::string> headers{std::string("g")};
    stream.Header(headers);
    std::vector<TypedValue> result{TypedValue(v)};
    stream.Result(result);
    return true;
  }

  ~CPUPlan() {}
};

extern "C" PlanInterface<Stream> *produce() { return new CPUPlan(); }

extern "C" void destruct(PlanInterface<Stream> *p) { delete p; }
