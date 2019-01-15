/// @file

#pragma once

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "database/single_node_ha/state_delta_applier.hpp"
#include "durability/single_node_ha/state_delta.hpp"
#include "raft/config.hpp"
#include "raft/coordination.hpp"
#include "raft/log_entry.hpp"
#include "raft/raft_interface.hpp"
#include "raft/raft_rpc_messages.hpp"
#include "raft/replication_log.hpp"
#include "storage/common/kvstore/kvstore.hpp"
#include "transactions/type.hpp"
#include "utils/scheduler.hpp"

namespace raft {

using Clock = std::chrono::system_clock;
using TimePoint = std::chrono::system_clock::time_point;

enum class Mode { FOLLOWER, CANDIDATE, LEADER };

inline std::string ModeToString(const Mode &mode) {
  switch (mode) {
    case Mode::FOLLOWER:
      return "FOLLOWER";
    case Mode::CANDIDATE:
      return "CANDIDATE";
    case Mode::LEADER:
      return "LEADER";
  }
}

/// Class which models the behaviour of a single server within the Raft
/// cluster. The class is responsible for storing both volatile and
/// persistent internal state of the corresponding state machine as well
/// as performing operations that comply with the Raft protocol.
class RaftServer final : public RaftInterface {
 public:
  RaftServer() = delete;

  /// The implementation assumes that server IDs are unique integers between
  /// ranging from 1 to cluster_size.
  ///
  /// @param server_id ID of the current server.
  /// @param durbility_dir directory for persisted data.
  /// @param config raft configuration.
  /// @param coordination Abstraction for coordination between Raft servers.
  /// @param delta_applier Object which is able to apply state deltas to SM.
  /// @param reset_callback Function that is called on each Leader->Follower
  ///                       transition.
  RaftServer(uint16_t server_id, const std::string &durability_dir,
             const Config &config, raft::Coordination *coordination,
             database::StateDeltaApplier *delta_applier,
             std::function<void(void)> reset_callback,
             std::function<void(void)> no_op_create);

  /// Starts the RPC servers and starts mechanisms inside Raft protocol.
  void Start();

  /// Stops all threads responsible for the Raft protocol.
  void Shutdown();

  /// Retrieves the current term from persistent storage.
  ///
  /// @throws MissingPersistentDataException
  uint64_t CurrentTerm();

  /// Retrieves the ID of the server this server has voted for in
  /// the current term from persistent storage. Returns std::nullopt
  /// if such server doesn't exist.
  std::experimental::optional<uint16_t> VotedFor();

  /// Retrieves the log entries from persistent storage. The log is 1-indexed
  /// in order to be consistent with the paper. If the Log isn't present in
  /// persistent storage, an empty Log will be created.
  std::vector<LogEntry> Log();

  /// Append to the log a list of batched state deltasa that are ready to be
  /// replicated.
  void AppendToLog(const tx::TransactionId &tx_id,
                   const std::vector<database::StateDelta> &deltas);

  /// Emplace a single StateDelta to the corresponding batch. If the StateDelta
  /// marks the transaction end, it will replicate the log accorss the cluster.
  void Emplace(const database::StateDelta &delta) override;

  /// Checks if the transaction with the given transaction id can safely be
  /// committed in local storage.
  bool SafeToCommit(const tx::TransactionId &tx_id) override;

  /// Returns true if the current servers mode is LEADER. False otherwise.
  bool IsLeader() override;

  void GarbageCollectReplicationLog(const tx::TransactionId &tx_id);

 private:
  /// Buffers incomplete Raft logs.
  ///
  /// A Raft log is considered to be complete if it ends with a StateDelta
  /// that represents transaction commit.
  /// LogEntryBuffer will be used instead of WriteAheadLog. We don't need to
  /// persist logs until we receive a majority vote from the Raft cluster, and
  /// apply the to our local state machine(storage).
  class LogEntryBuffer final {
   public:
    LogEntryBuffer() = delete;

    explicit LogEntryBuffer(RaftServer *raft_server);

    void Enable();

    /// Disable all future insertions in the buffer.
    ///
    /// Note: this will also clear all existing logs from buffers.
    void Disable();

    /// Insert a new StateDelta in logs.
    ///
    /// If the StateDelta type is `TRANSACTION_COMMIT` it will start
    /// replicating, and if the type is `TRANSACTION_ABORT` it will delete the
    /// log from buffer.
    void Emplace(const database::StateDelta &delta);

   private:
    bool enabled_{false};
    mutable std::mutex buffer_lock_;
    std::unordered_map<tx::TransactionId, std::vector<database::StateDelta>>
        logs_;

    RaftServer *raft_server_{nullptr};
  };

  mutable std::mutex lock_;  ///< Guards all internal state.

  //////////////////////////////////////////////////////////////////////////////
  // volatile state on all servers
  //////////////////////////////////////////////////////////////////////////////

  Config config_;                        ///< Raft config.
  Coordination *coordination_{nullptr};  ///< Cluster coordination.
  database::StateDeltaApplier *delta_applier_{nullptr};
  std::unique_ptr<ReplicationLog> rlog_{nullptr};

  std::atomic<Mode> mode_;  ///< Server's current mode.
  uint16_t server_id_;      ///< ID of the current server.
  uint64_t commit_index_;   ///< Index of the highest known committed entry.
  uint64_t last_applied_;   ///< Index of the highest applied entry to SM.

  /// Raft log entry buffer.
  ///
  /// LogEntryBuffer buffers Raft logs until a log is complete and ready for
  /// replication. This doesn't have to persist, if something fails before a
  /// log is ready for replication it will be discarded anyway.
  LogEntryBuffer log_entry_buffer_{this};

  std::vector<std::thread> peer_threads_;  ///< One thread per peer which
                                           ///< handles outgoing RPCs.

  std::condition_variable state_changed_;  ///< Notifies all peer threads on
                                           ///< relevant state change.

  std::thread no_op_issuer_thread_;  ///< Thread responsible for issuing no-op
                                     ///< command on leader change.

  std::condition_variable leader_changed_;  ///< Notifies the
                                            ///< no_op_issuer_thread that a new
                                            ///< leader has been elected.

  bool exiting_ = false;  ///< True on server shutdown.

  //////////////////////////////////////////////////////////////////////////////
  // volatile state on followers and candidates
  //////////////////////////////////////////////////////////////////////////////

  std::thread election_thread_;  ///< Timer thread for triggering elections.
  TimePoint next_election_;      ///< Next election `TimePoint`.

  std::condition_variable election_change_;  ///> Used to notify election_thread
                                             ///> on next_election_ change.

  std::mt19937_64 rng_ = std::mt19937_64(std::random_device{}());

  //////////////////////////////////////////////////////////////////////////////
  // volatile state on candidates
  //////////////////////////////////////////////////////////////////////////////

  uint16_t granted_votes_;
  std::vector<bool> vote_requested_;

  //////////////////////////////////////////////////////////////////////////////
  // volatile state on leaders
  //////////////////////////////////////////////////////////////////////////////

  std::vector<uint64_t> next_index_;  ///< for each server, index of the next
                                      ///< log entry to send to that server.

  std::vector<uint64_t> match_index_;  ///< for each server, index of the
                                       ///< highest log entry known to be
                                       ///< replicated on server.

  std::vector<TimePoint> next_heartbeat_;  ///< for each server, time point for
                                           ///< the next heartbeat.
  std::vector<TimePoint> backoff_until_;   ///< backoff for each server.

  //////////////////////////////////////////////////////////////////////////////
  // persistent state on all servers
  //
  // Persistent data consists of:
  //   - uint64_t current_term -- latest term server has seen.
  //   - uint16_t voted_for    -- candidate_id that received vote in current
  //                              term (null if none).
  //   - vector<LogEntry> log  -- log entries.
  //////////////////////////////////////////////////////////////////////////////

  storage::KVStore disk_storage_;

  /// Callback that needs to be called to reset the db state.
  std::function<void(void)> reset_callback_;

  /// Callback that creates a new transaction with NO_OP StateDelta.
  std::function<void(void)> no_op_create_callback_;

  /// Makes a transition to a new `raft::Mode`.
  ///
  /// throws InvalidTransitionException when transitioning between incompatible
  ///                                   `raft::Mode`s.
  void Transition(const raft::Mode &new_mode);

  /// Updates the current term.
  void UpdateTerm(uint64_t new_term);

  /// Tries to advance the commit index on a leader.
  void AdvanceCommitIndex();

  /// Recovers from persistent storage. This function should be called from
  /// the constructor before the server starts with normal operation.
  void Recover();

  /// Sends Entries to peer. This function should only be called in leader
  /// mode.
  ///
  /// @param peer_id ID of the peer which receives entries.
  /// @param lock Lock from the peer thread (released while waiting for
  ///             response)
  void SendEntries(uint16_t peer_id, std::unique_lock<std::mutex> &lock);

  /// Main function of the `election_thread_`. It is responsible for
  /// transition to CANDIDATE mode when election timeout elapses.
  void ElectionThreadMain();

  /// Main function of the thread that handles outgoing RPCs towards a
  /// specified node within the Raft cluster.
  ///
  /// @param peer_id - ID of a receiving node in the cluster.
  void PeerThreadMain(uint16_t peer_id);

  /// Issues no-op command when a new leader is elected. This is done to
  /// force the Raft protocol to commit logs from previous terms that
  /// have been replicated on a majority of peers.
  void NoOpIssuerThreadMain();

  /// Sets the `TimePoint` for next election.
  void SetNextElectionTimePoint();

  /// Checks if the current server obtained enough votes to become a leader.
  bool HasMajortyVote();

  /// Returns relevant metadata about the last entry in this server's Raft Log.
  /// More precisely, returns a pair consisting of an index of the last entry
  /// in the log and the term of the last entry in the log.
  ///
  /// @return std::pair<last_log_index, last_log_term>
  std::pair<uint64_t, uint64_t> LastEntryData();

  /// Checks whether Raft log of server A is at least as up-to-date as the Raft
  /// log of server B. This is strictly defined in Raft paper 5.4.
  ///
  /// @param last_log_index_a - Index of server A's last log entry.
  /// @param last_log_term_a  - Term of server A's last log entry.
  /// @param last_log_index_b - Index of server B's last log entry.
  /// @param last_log_term_b  - Term of server B's last log entry.
  bool AtLeastUpToDate(uint64_t last_log_index_a, uint64_t last_log_term_a,
                       uint64_t last_log_index_b, uint64_t last_log_term_b);

  /// Checks whether the current server got a reply from "future", i.e. reply
  /// with a higher term. If so, the current server falls back to follower mode
  /// and updates its current term.
  ///
  /// @param reply_term Term from RPC response.
  /// @return true if the current server's term lags behind.
  bool OutOfSync(uint64_t reply_term);

  /// Deletes log entries with indexes that are greater or equal to the given
  /// starting index.
  ///
  /// @param starting_index Smallest index which will be deleted from the Log.
  ///                       Also, a friendly remainder that log entries are
  ///                       1-indexed.
  void DeleteLogSuffix(int starting_index);

  /// Stores log entries with indexes that are greater or equal to the given
  /// starting index into a provided container. If the starting index is
  /// greater than the log size, nothing will be stored in the provided
  /// container.
  ///
  /// @param starting_index Smallest index which will be stored.
  /// @param entries The container which will store the wanted suffix.
  void GetLogSuffix(int starting_index, std::vector<raft::LogEntry> &entries);

  /// Appends new log entries to Raft log. Note that this function is not
  /// smart in any way, i.e. the caller should make sure that it's safe
  /// to call this function. This function also updates this server's commit
  /// index if necessary.
  ///
  /// @param leader_commit_index - Used to update local commit index.
  /// @param starting_index - Index in the log from which we start to append.
  /// @param new_entries - New `LogEntry` instances to be appended in the log.
  void AppendLogEntries(uint64_t leader_commit_index, uint64_t starting_index,
                        const std::vector<LogEntry> &new_entries);

  /// Serializes Raft log into `std::string`.
  std::string SerializeLog(const std::vector<LogEntry> &log);

  /// Deserializes Raft log from `std::string`.
  std::vector<LogEntry> DeserializeLog(const std::string &serialized_log);

  void ResetReplicationLog();
};
}  // namespace raft