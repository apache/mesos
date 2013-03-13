/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __SLAVE_STATE_HPP__
#define __SLAVE_STATE_HPP__

#include "stout/foreach.hpp"
#include "stout/hashmap.hpp"
#include "stout/hashset.hpp"
#include "stout/strings.hpp"
#include "stout/utils.hpp"

#include "common/type_utils.hpp"

#include "messages/messages.hpp"

#include "process/pid.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace state {

// SlaveState stores the information about the frameworks, executors and
// tasks running on this slave.
struct SlaveState
{
  struct FrameworkState
  {
    // TODO(vinod): Keep track of the latest run.
    struct RunState
    {
      struct ExecutorState
      {
        hashset<TaskID> tasks;
      };

      hashmap<UUID, ExecutorState> runs;
    };

    hashmap<ExecutorID, RunState> executors;
  };

  SlaveID slaveId;
  std::string slaveMetaDir;
  hashmap<FrameworkID, FrameworkState> frameworks;
};


// Parses the slave's work directory rooted at 'rootDir' and re-builds the
// the slave state.
SlaveState parse(const std::string& rootDir, const SlaveID& slaveId);

// Helper function to checkpoint a string message to disk.
void checkpoint(const std::string& path, const std::string& message);

// Helper function to checkpoint a protobuf message to disk.
void checkpoint(
    const std::string& path,
    const google::protobuf::Message& message);

} // namespace state {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_STATE_HPP__
