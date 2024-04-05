// Copyright 2024 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#pragma once

#include <memory>
#include <type_traits>

#include "query/common.hpp"
#include "query/frontend/semantic/symbol_table.hpp"
#include "query/metadata.hpp"
#include "query/parameters.hpp"
#include "query/plan/profile.hpp"
#include "query/trigger.hpp"
#include "utils/async_timer.hpp"

#include "query/frame_change.hpp"

namespace memgraph::query {

enum class TransactionStatus {
  IDLE,
  ACTIVE,
  VERIFYING,
  TERMINATED,
  STARTED_COMMITTING,
  STARTED_ROLLBACK,
};

struct EvaluationContext {
  /// Memory for allocations during evaluation of a *single* Pull call.
  ///
  /// Although the assigned memory may live longer than the duration of a Pull
  /// (e.g. memory is the same as the whole execution memory), you have to treat
  /// it as if the lifetime is only valid during the Pull.
  utils::MemoryResource *memory{utils::NewDeleteResource()};
  int64_t timestamp{-1};
  Parameters parameters{};
  /// All properties indexable via PropertyIx
  std::vector<storage::PropertyId> properties{};
  /// All labels indexable via LabelIx
  std::vector<storage::LabelId> labels{};
  /// All counters generated by `counter` function, mutable because the function
  /// modifies the values
  mutable std::unordered_map<std::string, int64_t> counters{};
};

inline std::vector<storage::PropertyId> NamesToProperties(const std::vector<std::string> &property_names,
                                                          DbAccessor *dba) {
  std::vector<storage::PropertyId> properties;
  properties.reserve(property_names.size());
  for (const auto &name : property_names) {
    properties.push_back(dba->NameToProperty(name));
  }
  return properties;
}

inline std::vector<storage::LabelId> NamesToLabels(const std::vector<std::string> &label_names, DbAccessor *dba) {
  std::vector<storage::LabelId> labels;
  labels.reserve(label_names.size());
  for (const auto &name : label_names) {
    labels.push_back(dba->NameToLabel(name));
  }
  return labels;
}

struct ExecutionContext {
  DbAccessor *db_accessor{nullptr};
  SymbolTable symbol_table;
  EvaluationContext evaluation_context;
  std::atomic<bool> *is_shutting_down{nullptr};
  std::atomic<TransactionStatus> *transaction_status{nullptr};
  bool is_profile_query{false};
  std::chrono::duration<double> profile_execution_time;
  plan::ProfilingStats stats;
  plan::ProfilingStats *stats_root{nullptr};
  ExecutionStats execution_stats;
  TriggerContextCollector *trigger_context_collector{nullptr};
  FrameChangeCollector *frame_change_collector{nullptr};
  std::shared_ptr<utils::AsyncTimer> timer;
  std::shared_ptr<QueryUserOrRole> user_or_role;
#ifdef MG_ENTERPRISE
  std::unique_ptr<FineGrainedAuthChecker> auth_checker{nullptr};
#endif
};

static_assert(std::is_move_assignable_v<ExecutionContext>, "ExecutionContext must be move assignable!");
static_assert(std::is_move_constructible_v<ExecutionContext>, "ExecutionContext must be move constructible!");

inline auto MustAbort(const ExecutionContext &context) noexcept -> AbortReason {
  if (context.transaction_status != nullptr &&
      context.transaction_status->load(std::memory_order_acquire) == TransactionStatus::TERMINATED) {
    return AbortReason::TERMINATED;
  }
  if (context.is_shutting_down != nullptr && context.is_shutting_down->load(std::memory_order_acquire)) {
    return AbortReason::SHUTDOWN;
  }
  if (context.timer && context.timer->IsExpired()) {
    return AbortReason::TIMEOUT;
  }
  return AbortReason::NO_ABORT;
}

inline plan::ProfilingStatsWithTotalTime GetStatsWithTotalTime(const ExecutionContext &context) {
  return plan::ProfilingStatsWithTotalTime{context.stats, context.profile_execution_time};
}

}  // namespace memgraph::query
