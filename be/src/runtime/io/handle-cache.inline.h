// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <tuple>

#include "runtime/io/handle-cache.h"
#include "runtime/io/hdfs-monitored-ops.h"
#include "util/hash-util.h"
#include "util/impalad-metrics.h"
#include "util/metrics.h"
#include "util/time.h"

#ifndef IMPALA_RUNTIME_DISK_IO_MGR_HANDLE_CACHE_INLINE_H
#define IMPALA_RUNTIME_DISK_IO_MGR_HANDLE_CACHE_INLINE_H

namespace impala {
namespace io {

HdfsFileHandle::~HdfsFileHandle() {
  if (hdfs_file_ != nullptr && fs_ != nullptr) {
    VLOG_FILE << "hdfsCloseFile() fid=" << hdfs_file_;
    hdfsCloseFile(fs_, hdfs_file_); // TODO: check return code
  }
  fs_ = nullptr;
  fname_ = nullptr;
  hdfs_file_ = nullptr;
}

Status HdfsFileHandle::Init(HdfsMonitor* monitor) {
  Status status = monitor->OpenHdfsFileWithTimeout(fs_, fname_, O_RDONLY, 0,
      &hdfs_file_);
  // fname_ is no longer needed, null it out
  fname_ = nullptr;
  return status;
}

CachedHdfsFileHandle::CachedHdfsFileHandle(const hdfsFS& fs, const string* fname,
    int64_t mtime)
  : HdfsFileHandle(fs, fname, mtime) {
  ImpaladMetrics::IO_MGR_NUM_CACHED_FILE_HANDLES->Increment(1L);
}

CachedHdfsFileHandle::~CachedHdfsFileHandle() {
  ImpaladMetrics::IO_MGR_NUM_CACHED_FILE_HANDLES->Increment(-1L);
}

FileHandleCache::FileHandleCache(size_t capacity,
    size_t num_partitions, uint64_t unused_handle_timeout_secs, HdfsMonitor* hdfs_monitor)
  : cache_partitions_(num_partitions),
  unused_handle_timeout_secs_(unused_handle_timeout_secs),
  hdfs_monitor_(hdfs_monitor) {
  DCHECK_GT(num_partitions, 0);
  size_t remainder = capacity % num_partitions;
  size_t base_capacity = capacity / num_partitions;
  size_t partition_capacity = (remainder > 0 ? base_capacity + 1 : base_capacity);
  for (FileHandleCachePartition& p : cache_partitions_) {
    p.size = 0;
    p.capacity = partition_capacity;
  }
}

FileHandleCache::~FileHandleCache() {
  shut_down_promise_.Set(true);
  if (eviction_thread_ != nullptr) eviction_thread_->Join();
}

Status FileHandleCache::Init() {
  return Thread::Create("disk-io-mgr-handle-cache", "File Handle Timeout",
      &FileHandleCache::EvictHandlesLoop, this, &eviction_thread_);
}

Status FileHandleCache::GetFileHandle(
    const hdfsFS& fs, std::string* fname, int64_t mtime, bool require_new_handle,
    CachedHdfsFileHandle** handle_out, bool* cache_hit) {
  DCHECK_GT(mtime, 0);
  // Hash the key and get appropriate partition
  int index = HashUtil::Hash(fname->data(), fname->size(), 0) % cache_partitions_.size();
  FileHandleCachePartition& p = cache_partitions_[index];

  // If this requires a new handle, skip to the creation codepath. Otherwise,
  // find an unused entry with the same mtime
  if (!require_new_handle) {
    std::lock_guard<SpinLock> g(p.lock);
    typename MapType::iterator map_it = p.cache.find(*fname);

    // When picking a cached entry, always pick earlier entries first. This allows
    // excessive entries for a file to age out. For example, if there are three entries
    // for a file and only one is used at a time, only the first will be used and the
    // other two can age out.
    if (map_it != p.cache.end()) {
      for (typename FileHandleListType::iterator fh_it = map_it->second.fh_list.begin();
           fh_it != map_it->second.fh_list.end(); ++fh_it) {
        DCHECK(fh_it != map_it->second.fh_list.end());
        CachedHdfsFileHandle fh = *fh_it;
        if (!fh.in_use && fh.mtime() == mtime) {
          // This element is currently in the lru_list, which means that lru_entry must
          // be an iterator pointing into the lru_list.
          DCHECK(fh_it->lru_list_hook_.is_linked());
          // Remove the element from the lru_list and designate that it is not on
          // the lru_list by resetting its iterator to point to the end of the list.
          p.lru_list.erase(p.lru_list.iterator_to(*fh_it));
          *cache_hit = true;
          fh_it->in_use = true;
          *handle_out = &fh;
          return Status::OK();
        }
      }
    } else {
      // There is no entry for this file name in the map. Create a new list and add it to
      // the file name in the map.
      // FileHandleStruct fh_struct;
      p.cache.emplace(std::make_pair(*fname, FileHandleStruct()));
    }
  }

  // There was no entry that was free or caller asked for a new handle
  // Opening a file handle requires talking to the NameNode, so construct
  // the file handle without holding the lock to reduce contention.
  *cache_hit = false;
  // Create a new file handle
  CachedHdfsFileHandle new_fh(fs, fname, mtime);
  RETURN_IF_ERROR(new_fh.Init(hdfs_monitor_));

  // Get the lock and create/move the new entry into the map
  // This entry is new and will be immediately used. Place it as the first entry
  // for this file in the list. The ordering is largely unimportant if all the
  // existing entries are in use. However, if require_new_handle is true, there may be
  // unused entries, so it would make more sense to insert the new entry at the front.
  std::lock_guard<SpinLock> g(p.lock);
  typename MapType::iterator new_it = p.cache.find(*fname);
  new_fh.fh_struct = &new_it->second;
  new_it->second.fh_list.push_front(new_fh);
  ++p.size;
  if (p.size > p.capacity) EvictHandles(p);
  CachedHdfsFileHandle* new_elem = &(new_it->second.fh_list.front());
  DCHECK(!new_elem->in_use);
  new_elem->in_use = true;
  *handle_out = new_elem;
  return Status::OK();
}

void FileHandleCache::ReleaseFileHandle(std::string* fname,
    CachedHdfsFileHandle* fh, bool destroy_handle) {
  DCHECK(fh != nullptr);
  // Hash the key and get appropriate partition
  int index = HashUtil::Hash(fname->data(), fname->size(), 0) % cache_partitions_.size();
  FileHandleCachePartition& p = cache_partitions_[index];
  std::lock_guard<SpinLock> g(p.lock);
  typename MapType::iterator map_it = p.cache.find(*fname);

  // Search in the list of file handles for this file.
  // TODO: This can be optimized by maintaining some state in the file handle about
  // its location in the map.
  typename FileHandleListType::iterator fh_entry_it;
  for (fh_entry_it = map_it->second.fh_list.begin();
       fh_entry_it != map_it->second.fh_list.end(); ++fh_entry_it) {
    if (&(*fh_entry_it) == fh) break;
  }
  DCHECK(fh_entry_it != map_it->second.fh_list.end());

  // This file handle is no longer referenced
  CachedHdfsFileHandle* release_elem = &(*fh_entry_it);
  DCHECK(release_elem->in_use);
  release_elem->in_use = false;
  if (destroy_handle) {
    --p.size;
    map_it->second.fh_list.erase(fh_entry_it);
    return;
  }
  // Hdfs can use some memory for readahead buffering. Calling unbuffer reduces
  // this buffering so that the file handle takes up less memory when in the cache.
  // If unbuffering is not supported, then hdfsUnbufferFile() will return a non-zero
  // return code, and we close the file handle and remove it from the cache.
  if (hdfsUnbufferFile(release_elem->file()) == 0) {
    // This FileHandleEntry must not be in the lru list already, because it was
    // in use. Verify this by checking that the lru_entry is pointing to the end,
    // which cannot be true for any element in the lru list.
    DCHECK(!release_elem->lru_list_hook_.is_linked());
    // Add this to the lru list, establishing links in both directions.

    p.lru_list.push_back(*release_elem);
    if (p.size > p.capacity) EvictHandles(p);
  } else {
    VLOG_FILE << "FS does not support file handle unbuffering, closing file="
              << fname;
    --p.size;
    map_it->second.fh_list.erase(fh_entry_it);
  }
}

void FileHandleCache::EvictHandlesLoop() {
  while (true) {
    for (FileHandleCachePartition& p : cache_partitions_) {
      std::lock_guard<SpinLock> g(p.lock);
      EvictHandles(p);
    }
    // This Get() will time out until shutdown, when the promise is set.
    bool timed_out;
    shut_down_promise_.Get(EVICT_HANDLES_PERIOD_MS, &timed_out);
    if (!timed_out) break;
  }
  // The promise must be set to true.
  DCHECK(shut_down_promise_.IsSet());
  DCHECK(shut_down_promise_.Get());
}

void FileHandleCache::EvictHandles(
    FileHandleCache::FileHandleCachePartition& p) {
  uint64_t now = MonotonicSeconds();
  uint64_t oldest_allowed_timestamp =
      now > unused_handle_timeout_secs_ ? now - unused_handle_timeout_secs_ : 0;
  while (p.lru_list.size() > 0) {
    // Peek at the oldest element
    CachedHdfsFileHandle oldest_fh = p.lru_list.front();
    auto oldest_fh_struct= oldest_fh.fh_struct;
    uint64_t oldest_entry_timestamp = oldest_fh.timestamp_seconds;
    // If the oldest element does not need to be aged out and the cache is not over
    // capacity, then we are done and there is nothing to evict.
    if (p.size <= p.capacity && (unused_handle_timeout_secs_ == 0 ||
        oldest_entry_timestamp >= oldest_allowed_timestamp)) {
      return;
    }
    // Evict the oldest element
    DCHECK(!oldest_fh.lru_list_hook_.is_linked());
    oldest_fh_struct->fh_list.erase(oldest_fh_struct->fh_list.iterator_to(oldest_fh));
    p.lru_list.pop_front();
    --p.size;
  }
}
}
}
#endif
