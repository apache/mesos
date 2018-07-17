// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __MESOS_ZOOKEEPER_WATCHER_HPP__
#define __MESOS_ZOOKEEPER_WATCHER_HPP__

#include <stdint.h>

#include <glog/logging.h>

#include <mesos/zookeeper/zookeeper.hpp>

#include <process/dispatch.hpp>


// A watcher which dispatches events to a process. Note that it is
// only "safe" to reuse an instance across ZooKeeper instances after a
// session expiration. TODO(benh): Add a 'reset/initialize' to the
// Watcher so that a single instance can be reused.
// NOTE: By the time the dispatched events are processed by 'pid',
// its session ID may have changed! Therefore, we pass the session ID
// for the event to allow the 'pid' Process to check for staleness.
template <typename T>
class ProcessWatcher : public Watcher
{
public:
  explicit ProcessWatcher(const process::PID<T>& _pid)
    : pid(_pid), reconnect(false) {}

  void process(
      int type,
      int state,
      int64_t sessionId,
      const std::string& path) override
  {
    if (type == ZOO_SESSION_EVENT) {
      if (state == ZOO_CONNECTED_STATE) {
        // Connected (initial or reconnect).
        process::dispatch(pid, &T::connected, sessionId, reconnect);
        // If this watcher gets reused then the next connected
        // event shouldn't be perceived as a reconnect.
        reconnect = false;
      } else if (state == ZOO_CONNECTING_STATE) {
        // The client library automatically reconnects, taking
        // into account failed servers in the connection string,
        // appropriately handling the "herd effect", etc.
        process::dispatch(pid, &T::reconnecting, sessionId);
        // TODO(benh): If this watcher gets reused then the next
        // connected event will be perceived as a reconnect, but it
        // should not.
        reconnect = true;
      } else if (state == ZOO_EXPIRED_SESSION_STATE) {
        process::dispatch(pid, &T::expired, sessionId);
        // If this watcher gets reused then the next connected
        // event shouldn't be perceived as a reconnect.
        reconnect = false;
      } else {
        LOG(FATAL) << "Unhandled ZooKeeper state (" << state << ")"
                   << " for ZOO_SESSION_EVENT";
      }
    } else if (type == ZOO_CHILD_EVENT) {
      process::dispatch(pid, &T::updated, sessionId, path);
    } else if (type == ZOO_CHANGED_EVENT) {
      process::dispatch(pid, &T::updated, sessionId, path);
    } else if (type == ZOO_CREATED_EVENT) {
      process::dispatch(pid, &T::created, sessionId, path);
    } else if (type == ZOO_DELETED_EVENT) {
      process::dispatch(pid, &T::deleted, sessionId, path);
    } else {
      LOG(FATAL) << "Unhandled ZooKeeper event (" << type << ")"
                 << " in state (" << state << ")";
    }
  }

private:
  const process::PID<T> pid;
  bool reconnect;
};

#endif // __MESOS_ZOOKEEPER_WATCHER_HPP__
