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

#ifndef __SLAVE_HPP__
#define __SLAVE_HPP__

#include <process/process.hpp>
#include <process/protobuf.hpp>

#include "isolation_module.hpp"
#include "state.hpp"

#include "common/resources.hpp"
#include "common/hashmap.hpp"
#include "common/uuid.hpp"

#include "configurator/configurator.hpp"

#include "messages/messages.hpp"


namespace mesos { namespace internal { namespace slave {

using namespace process;

struct Framework;
struct Executor;

// TODO(benh): Also make configuration options be constants.

const double EXECUTOR_SHUTDOWN_TIMEOUT_SECONDS = 5.0;
const double STATUS_UPDATE_RETRY_INTERVAL_SECONDS = 10.0;


// Slave process.
class Slave : public ProtobufProcess<Slave>
{
public:
  Slave(const Configuration& conf,
        bool local,
        IsolationModule *isolationModule);

  Slave(const Resources& resources,
        bool local,
        IsolationModule* isolationModule);

  virtual ~Slave();

  static void registerOptions(Configurator* configurator);

  Promise<state::SlaveState*> getState();

  void newMasterDetected(const UPID& pid);
  void noMasterDetected();
  void masterDetectionFailure();
  void registered(const SlaveID& slaveId);
  void reregistered(const SlaveID& slaveId);
  void runTask(const FrameworkInfo& frameworkInfo,
               const FrameworkID& frameworkId,
               const std::string& pid,
               const TaskDescription& task);
  void killTask(const FrameworkID& frameworkId,
                const TaskID& taskId);
  void shutdownFramework(const FrameworkID& frameworkId);
  void schedulerMessage(const SlaveID& slaveId,
			const FrameworkID& frameworkId,
			const ExecutorID& executorId,
			const std::string& data);
  void updateFramework(const FrameworkID& frameworkId,
                       const std::string& pid);
  void statusUpdateAcknowledgement(const SlaveID& slaveId,
                                   const FrameworkID& frameworkId,
                                   const TaskID& taskId,
                                   const std::string& uuid);
  void registerExecutor(const FrameworkID& frameworkId,
                        const ExecutorID& executorId);
  void statusUpdate(const StatusUpdate& update);
  void executorMessage(const SlaveID& slaveId,
                       const FrameworkID& frameworkId,
                       const ExecutorID& executorId,
                       const std::string& data);
  void ping();
  void exited();

  void statusUpdateTimeout(const FrameworkID& frameworkId, const UUID& uuid);

  void executorStarted(const FrameworkID& frameworkId,
                       const ExecutorID& executorId,
                       pid_t pid);

  void executorExited(const FrameworkID& frameworkId,
                      const ExecutorID& executorId,
                      int status);

protected:
  virtual void operator () ();

  void initialize();

  // Helper routine to lookup a framework.
  Framework* getFramework(const FrameworkID& frameworkId);

  // Shut down an executor. This is a two phase process. First, an
  // executor receives a shut down message (shut down phase), then
  // after a configurable timeout the slave actually forces a kill
  // (kill phase, via the isolation module) if the executor has not
  // exited.
  void shutdownExecutor(Framework* framework, Executor* executor);

  // Handle the second phase of shutting down an executor for those
  // executors that have not properly shutdown within a timeout.
  void shutdownExecutorTimeout(const FrameworkID& frameworkId,
                               const ExecutorID& executorId,
                               const UUID& uuid);

//   // Create a new status update stream.
//   StatusUpdates* createStatusUpdateStream(const StatusUpdateStreamID& streamId,
//                                           const string& directory);

//   StatusUpdates* getStatusUpdateStream(const StatusUpdateStreamID& streamId);

  // Helper function for generating a unique work directory for this
  // framework/executor pair (non-trivial since a framework/executor
  // pair may be launched more than once on the same slave).
  std::string getUniqueWorkDirectory(const FrameworkID& frameworkId,
                                     const ExecutorID& executorId);

private:
  // TODO(benh): Better naming and name scope for these http handlers.
  Promise<HttpResponse> http_info_json(const HttpRequest& request);
  Promise<HttpResponse> http_frameworks_json(const HttpRequest& request);
  Promise<HttpResponse> http_tasks_json(const HttpRequest& request);
  Promise<HttpResponse> http_stats_json(const HttpRequest& request);
  Promise<HttpResponse> http_vars(const HttpRequest& request);

  const Configuration conf;

  bool local;

  SlaveID id;
  SlaveInfo info;

  UPID master;

  Resources resources;

  hashmap<FrameworkID, Framework*> frameworks;

  IsolationModule* isolationModule;

  // Statistics (initialized in Slave::initialize).
  struct {
    uint64_t tasks[TaskState_ARRAYSIZE];
    uint64_t validStatusUpdates;
    uint64_t invalidStatusUpdates;
    uint64_t validFrameworkMessages;
    uint64_t invalidFrameworkMessages;
  } stats;

  double startTime;

//   typedef std::pair<FrameworkID, TaskID> StatusUpdateStreamID;
//   hashmap<std::pair<FrameworkID, TaskID>, StatusUpdateStream*> statusUpdateStreams;

//   hashmap<std::pair<FrameworkID, TaskID>, PendingStatusUpdate> pendingUpdates;
};

}}}

#endif // __SLAVE_HPP__
