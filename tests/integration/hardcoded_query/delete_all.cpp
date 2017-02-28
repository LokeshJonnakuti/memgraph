#include <iostream>
#include <string>

#include "query/plan_interface.hpp"
#include "using.hpp"

using std::cout;
using std::endl;

// Query: MATCH (n) DETACH DELETE n

class CPUPlan : public PlanInterface<Stream> {
 public:
  bool run(GraphDbAccessor &db_accessor, const TypedValueStore<> &args,
           Stream &stream) {
    for (auto v : db_accessor.vertices()) db_accessor.detach_remove_vertex(v);
    stream.write_empty_fields();
    stream.write_meta("rw");
    db_accessor.transaction_.commit();
    return true;
  }

  ~CPUPlan() {}
};

extern "C" PlanInterface<Stream> *produce() { return new CPUPlan(); }

extern "C" void destruct(PlanInterface<Stream> *p) { delete p; }
