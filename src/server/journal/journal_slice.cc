// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/journal/journal_slice.h"

#include <absl/container/inlined_vector.h>
#include <absl/flags/flag.h>
#include <absl/strings/escaping.h>
#include <absl/strings/str_cat.h>
#include <fcntl.h>

#include <filesystem>

#include "base/function2.hpp"
#include "base/logging.h"
#include "server/journal/serializer.h"

ABSL_FLAG(uint32_t, shard_repl_backlog_len, 1 << 10,
          "The length of the circular replication log per shard");

namespace dfly {
namespace journal {
using namespace std;
using namespace util;

JournalSlice::JournalSlice() {
}

JournalSlice::~JournalSlice() {
}

void JournalSlice::Init() {
  if (ring_buffer_)  // calling this function multiple times is allowed and it's a no-op.
    return;

  ring_buffer_.emplace(2);
}

bool JournalSlice::IsLSNInBuffer(LSN lsn) const {
  DCHECK(ring_buffer_);

  if (ring_buffer_->empty()) {
    return false;
  }
  return (*ring_buffer_)[0].lsn <= lsn && lsn <= ((*ring_buffer_)[ring_buffer_->size() - 1].lsn);
}

std::string_view JournalSlice::GetEntry(LSN lsn) const {
  DCHECK(ring_buffer_ && IsLSNInBuffer(lsn));
  auto start = (*ring_buffer_)[0].lsn;
  DCHECK((*ring_buffer_)[lsn - start].lsn == lsn);
  return (*ring_buffer_)[lsn - start].data;
}

void JournalSlice::SetFlushMode(bool allow_flush) {
  DCHECK(allow_flush != enable_journal_flush_);
  enable_journal_flush_ = allow_flush;
  if (allow_flush) {
    // This lock is never blocking because it contends with UnregisterOnChange, which is cpu only.
    // Hence this lock prevents the UnregisterOnChange to start running in the middle of
    // SetFlushMode.
    std::shared_lock lk(cb_mu_);
    for (auto k_v : journal_consumers_arr_) {
      k_v.second->ThrottleIfNeeded();
    }
  }
}

void JournalSlice::AddLogRecord(const Entry& entry) {
  DCHECK(ring_buffer_);

  JournalItem item;
  {
    FiberAtomicGuard fg;
    item.opcode = entry.opcode;
    item.lsn = lsn_++;
    item.cmd = entry.payload.cmd;
    item.slot = entry.slot;

    io::BufSink buf_sink{&ring_serialize_buf_};
    JournalWriter writer{&buf_sink};
    writer.Write(entry);

    item.data = io::View(ring_serialize_buf_.InputBuffer());
    ring_serialize_buf_.Clear();
    VLOG(2) << "Writing item [" << item.lsn << "]: " << entry.ToString();
  }

  CallOnChange(item);
}

void JournalSlice::CallOnChange(const JournalItem& item) {
  // This lock is never blocking because it contends with UnregisterOnChange, which is cpu only.
  // Hence this lock prevents the UnregisterOnChange to start running in the middle of CallOnChange.
  // CallOnChange is atomic if JournalSlice::SetFlushMode(false) is called before.
  std::shared_lock lk(cb_mu_);
  for (auto k_v : journal_consumers_arr_) {
    k_v.second->ConsumeJournalChange(item);
  }
  if (enable_journal_flush_) {
    for (auto k_v : journal_consumers_arr_) {
      k_v.second->ThrottleIfNeeded();
    }
  }
}

uint32_t JournalSlice::RegisterOnChange(JournalConsumerInterface* consumer) {
  // mutex lock isn't needed due to iterators are not invalidated
  uint32_t id = next_cb_id_++;
  journal_consumers_arr_.emplace_back(id, consumer);
  return id;
}

void JournalSlice::UnregisterOnChange(uint32_t id) {
  // we need to wait until callback is finished before remove it
  lock_guard lk(cb_mu_);
  auto it = find_if(journal_consumers_arr_.begin(), journal_consumers_arr_.end(),
                    [id](const auto& e) { return e.first == id; });
  CHECK(it != journal_consumers_arr_.end());
  journal_consumers_arr_.erase(it);
}

}  // namespace journal
}  // namespace dfly
