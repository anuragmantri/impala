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

#pragma once

#include <fstream>
#include <mutex>

#include "common/status.h"

namespace impala {

class LeaderElectionMgr {
 public:
  LeaderElectionMgr(const std::string& shared_dir);

  /// Initializes the shared directory and creates the initial shared file. If the dir
  /// does not already exist, it will be created. This function is not thread safe and
  /// should only be called once.
  Status Init(bool& is_leader);

  Status CheckAndSetLeader(bool& is_leader);

  Status RenewLease();

  int64_t GetLeaseExpiryMs();

  int64_t GetCurrentTime();

 private:
  /// Lock to protect the shared file.
  std::mutex shared_file_lock_;

  /// Shared directory name.
  std::string shared_dir_;

  /// Current shared file name.
  std::string shared_file_name_;

  std::ofstream shared_file_;

  int64_t lease_expiry_ms_ = 0;

  /// Generates and sets a new log file name that is based off the current system time.
  /// The format will be: <UTC timestamp>
  void GenerateFileName();

  Status FlushInternal();
};
}

