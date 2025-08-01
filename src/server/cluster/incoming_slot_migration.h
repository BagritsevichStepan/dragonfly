// Copyright 2023, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//
#pragma once

#include "helio/util/fiber_socket_base.h"
#include "server/cluster/cluster_defs.h"
#include "server/common.h"

namespace dfly {
class Service;
}

namespace dfly::cluster {
class ClusterShardMigration;

// The main entity on the target side that manage slots migration process
// Manage connections between the target and source node,
// manage migration process state and data
class IncomingSlotMigration {
 public:
  IncomingSlotMigration(std::string source_id, Service* se, SlotRanges slots);
  ~IncomingSlotMigration();

  // process data from FDLYMIGRATE FLOW cmd
  // executes until Stop called or connection closed
  void StartFlow(uint32_t shard, util::FiberSocketBase* source);

  // Waits until all flows got FIN opcode.
  // returns true if we joined false if timeout is readed
  // After Join we still can get data due to error situation
  [[nodiscard]] bool Join(long attempt);

  // Stop and join the migration, can be called even after migration is finished
  void Stop();

  // Init/Reinit migration
  void Init(uint32_t shards_num);

  MigrationState GetState() const {
    util::fb2::LockGuard lk(state_mu_);
    return state_;
  }

  const SlotRanges& GetSlots() const {
    return slots_;
  }

  const std::string& GetSourceID() const {
    return source_id_;
  }

  size_t ShardNum() const {
    return shard_flows_.size();
  }

  // Switch to  FATAL state and store error message
  void ReportFatalError(dfly::GenericError err) ABSL_LOCKS_EXCLUDED(state_mu_, error_mu_) {
    errors_count_.fetch_add(1, std::memory_order_relaxed);
    util::fb2::LockGuard lk_state(state_mu_);
    util::fb2::LockGuard lk_error(error_mu_);
    state_ = MigrationState::C_FATAL;
    last_error_ = std::move(err);
  }

  void ReportError(dfly::GenericError err) ABSL_LOCKS_EXCLUDED(error_mu_) {
    errors_count_.fetch_add(1, std::memory_order_relaxed);
    util::fb2::LockGuard lk(error_mu_);
    if (GetState() != MigrationState::C_FATAL)
      last_error_ = std::move(err);
  }

  std::string GetErrorStr() const ABSL_LOCKS_EXCLUDED(error_mu_) {
    util::fb2::LockGuard lk(error_mu_);
    return last_error_.Format();
  }

  size_t GetErrorsCount() const {
    return errors_count_.load(std::memory_order_relaxed);
  }

  size_t GetKeyCount() const;

  void Pause(bool pause);

 private:
  std::string source_id_;
  Service& service_;
  std::vector<std::unique_ptr<ClusterShardMigration>> shard_flows_;
  SlotRanges slots_;
  ExecutionState cntx_;

  mutable util::fb2::Mutex error_mu_;
  dfly::GenericError last_error_ ABSL_GUARDED_BY(error_mu_);
  std::atomic<size_t> errors_count_ = 0;

  mutable util::fb2::Mutex state_mu_;
  MigrationState state_ ABSL_GUARDED_BY(state_mu_) = MigrationState::C_CONNECTING;

  // when migration is finished we need to store number of migrated keys
  // because new request can add or remove keys and we get incorrect statistic
  size_t keys_number_ = 0;

  util::fb2::BlockingCounter bc_;
};

}  // namespace dfly::cluster
