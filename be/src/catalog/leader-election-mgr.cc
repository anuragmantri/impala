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

#include "catalog/leader-election-mgr.h"
#include <mutex>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/filesystem.hpp>
#include <gutil/strings/substitute.h>

#include "common/names.h"

using boost::filesystem::create_directory;
using boost::filesystem::exists;
using boost::filesystem::is_directory;
using boost::filesystem::directory_entry;
using boost::filesystem::directory_iterator;
using boost::posix_time::microsec_clock;
using boost::posix_time::ptime;
using boost::posix_time::time_from_string;
using namespace impala;

const ptime EPOCH = time_from_string("1970-01-01 00:00:00.000");
const int64_t LEASE_EXTENSTION_DURATION_MS = 5000;

Status InitSharedDir(const string& shared_dir) {
  if (!exists(shared_dir)) {
    LOG(INFO) << "ANURAG: Shared directory does not exist, creating: " << shared_dir;
    try {
      create_directory(shared_dir);
    } catch (const std::exception& e) { // Explicit std:: to distinguish from boost::
      LOG(ERROR) << "ANURAG: Could not create shared directory: "
                 << shared_dir << ", " << e.what();
      return Status("ANURAG: Failed to create shared directory");
    }
  }
  if (!is_directory(shared_dir)) {
    LOG(ERROR) << "ANURAG: Share directory path is not a directory ("
               << shared_dir << ")";
    return Status("ANURAG: Shared directory path is not a directory");
  }

  return Status::OK();
}

int64_t LeaderElectionMgr::GetLeaseExpiryMs() { return lease_expiry_ms_; }

LeaderElectionMgr::LeaderElectionMgr(const string& shared_dir)
    : shared_dir_(shared_dir) {
}

int64_t LeaderElectionMgr::GetCurrentTime() {
  return (microsec_clock::universal_time() - EPOCH).total_milliseconds();
}

Status LeaderElectionMgr::Init(bool& is_leader) {
  DCHECK(shared_file_name_.empty());
  if (exists(shared_dir_)) {
    for (directory_entry& x : directory_iterator(shared_dir_)) {
      auto path = x.path();
      shared_file_name_ = path.string();
      std::istringstream iss(path.stem().string());
      iss >> lease_expiry_ms_;
      // If lease is still active, this cannot be the leader.
      if (lease_expiry_ms_ < GetCurrentTime()) {
        RETURN_IF_ERROR(RenewLease());
        is_leader = true;
      } else {
        LOG(INFO) << "ANURAG: Found an unexpired lease. This catalog will not be leader.";
        is_leader = false;
      }
    }
  } else {
    RETURN_IF_ERROR(InitSharedDir(shared_dir_));
    GenerateFileName();
    RETURN_IF_ERROR(FlushInternal());
    RETURN_IF_ERROR(RenewLease());
    LOG(INFO) << "ANURAG: Shared file name created: " << shared_file_name_;
    LOG(INFO) << "ANURAG: Lease expires at: " << lease_expiry_ms_;
    is_leader = true;
  }
  return Status::OK();
}



Status LeaderElectionMgr::RenewLease() {
  if (lease_expiry_ms_ == 0) {
    // Initial expiry
    lease_expiry_ms_ = lease_expiry_ms_ + LEASE_EXTENSTION_DURATION_MS;
  } else {
    stringstream ss;
    int64_t new_lease =
        (microsec_clock::universal_time() - EPOCH).total_milliseconds() +
        LEASE_EXTENSTION_DURATION_MS;
    ss << shared_dir_ << "/" << new_lease;
    boost::filesystem::rename(shared_file_name_, ss.str());
    lease_expiry_ms_ = new_lease;
  }
  return Status::OK();
}

Status LeaderElectionMgr::CheckAndSetLeader(bool& is_leader) {
  // If lease expired, make this the file, rename the file and set new expiry.

  return Status::OK();
}

void LeaderElectionMgr::GenerateFileName() {
  stringstream ss;
  int64_t ms_since_epoch =
      (microsec_clock::universal_time() - EPOCH).total_milliseconds();
  lease_expiry_ms_ = ms_since_epoch;
  ss << shared_dir_ << "/" << ms_since_epoch;
  shared_file_name_ = ss.str();
}

Status LeaderElectionMgr::FlushInternal() {
  if (shared_file_.is_open()) {
    // flush() alone does not apparently fsync, but we actually want
    // the results to become visible, hence the close / reopen
    shared_file_.flush();
    shared_file_.close();
  }
  shared_file_.open(shared_file_name_.c_str(), std::ios_base::app | std::ios_base::out);
  if (!shared_file_.is_open()) return Status("Could not open shared file: " +
      shared_file_name_);
  LOG(INFO) << "ANURAG: Shared file created: " << shared_file_name_;
  return Status::OK();
}
