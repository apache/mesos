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

// TODO(benh): Optimize LearnedMessage (and the "commit" stage in
// general) by figuring out a way to not send the entire action
// contents a second time (should cut bandwidth used in half).

// TODO(benh): Provide a LearnRequest that requests more than one
// position at a time, and a LearnResponse that returns as many
// positions as it knows.

// TODO(benh): Implement background catchup: have a new replica that
// comes online become part of the group but don't respond to promises
// or writes until it has caught up! The advantage to becoming part of
// the group is that the new replica can see where the end of the log
// is in order to continue to catch up.

// TODO(benh): Add tests that deliberatly put the system in a state of
// inconsistency by doing funky things to the underlying logs. Figure
// out ways of bringing new replicas online that seem to check the
// consistency of the other replicas.

#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/lexical_cast.hpp>

#include <process/dispatch.hpp>
#include <process/process.hpp>
#include <process/run.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/fatal.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>

#include "zookeeper/zookeeper.hpp"

#include "log/coordinator.hpp"
#include "log/replica.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::log;

using namespace process;

using std::list;
using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;


// class Drop : public Filter
// {
// public:
//   Drop
//   virtual bool filter(Message* message)
//   {
//     return  == message->name;
//   }
// };


// class PeriodicFilter


char** args; // Command line arguments for doing a restart.


void restart()
{
  LOG(INFO) << "Restarting ...";
  execv(args[0], args);
  fatalerror("Failed to exec");
}


bool coordinate(Coordinator* coordinator,
                uint64_t id,
                int end,
                map<int, int> truncations)
{
  const int attempts = 3;

  uint64_t index;

  int attempt = 1;
  while (true) {
    Result<uint64_t> result = coordinator->elect(id);
    if (result.isError()) {
      restart();
    } else if (result.isNone()) {
      if (attempt == attempts) {
        restart();
      } else {
        attempt++;
        os::sleep(Seconds(1));
      }
    } else {
      CHECK_SOME(result);
      index = result.get();
      break;
    }
  }

  uint64_t value = 0;

  if (index != 0) {
    attempt = 1;
    while (true) {
      Result<list<pair<uint64_t, string> > > result =
        coordinator->read(index, index);
      if (result.isError()) {
        LOG(INFO) << "Restarting due to read error";
        restart();
      } else if (result.isNone()) {
        if (attempt == attempts) {
          LOG(INFO) << "Restarting after too many attempts";
          restart();
        } else {
          attempt++;
          os::sleep(Seconds(1));
        }
      } else {
        CHECK_SOME(result);
        const list<pair<uint64_t, string> >& list = result.get();
        if (list.size() != 1) {
          index--;
        } else {
          try {
            value = boost::lexical_cast<uint64_t>(list.front().second);
          } catch (boost::bad_lexical_cast&) {
            LOG(INFO) << "Restarting due to conversion error";
            restart();
          }
          break;
        }
      }
    }
  }

  value++;

  srand(time(NULL));

  int writes = rand() % 500;

  LOG(INFO) << "Attempting to do " << writes << " writes";

  attempt = 1;
  while (writes > 0 && value <= end) {
    if (truncations.count(value) > 0) {
      int to = truncations[value];
      Result<uint64_t> result = coordinator->truncate(to);
      if (result.isError()) {
        LOG(INFO) << "Restarting due to truncate error";
        restart();
      } else if (result.isNone()) {
        if (attempt == attempts) {
          LOG(INFO) << "Restarting after too many attempts";
          restart();
        } else {
          attempt++;
          os::sleep(Seconds(1));
          continue;
        }
      } else {
        CHECK_SOME(result);
        LOG(INFO) << "Truncated to " << to;
        os::sleep(Seconds(1));
        attempt = 1;
      }
    }

    Result<uint64_t> result = coordinator->append(stringify(value));
    if (result.isError()) {
      LOG(INFO) << "Restarting due to append error";
      restart();
    } else if (result.isNone()) {
      if (attempt == attempts) {
        LOG(INFO) << "Restarting after too many attempts";
        restart();
      } else {
        attempt++;
        os::sleep(Seconds(1));
      }
    } else {
      CHECK_SOME(result);
      LOG(INFO) << "Wrote " << value;
      os::sleep(Seconds(1));
      writes--;
      value++;
      attempt = 1;
    }
  }

  exit(0);
  return true;
}


class LogProcess : public Process<LogProcess>
{
public:
  LogProcess(int _quorum,
             const string& _file,
             const string& _servers,
             const string& _znode,
             int _end,
             const map<int, int>& _truncations);

  virtual ~LogProcess();

  // ZooKeeper events. TODO(*): Use a ZooKeeper listener?
  void connected();
  void reconnecting();
  void reconnected();
  void expired();
  void updated(const string& path);

protected:
  virtual void initialze();

private:
  // Updates the group.
  void regroup();

  // Runs an election.
  void elect();

  // ZooKeeper bits and pieces.
  string servers;
  string znode;
  ZooKeeper* zk;
  Watcher* watcher;

  // Size of quorum.
  int quorum;

  // Log file.
  string file;

  // Termination value (when to stop writing to the log).
  int end;

  // Truncation points.
  map<int, int> truncations;

  // Coordinator id.
  uint64_t id;

  // Whether or not the coordinator has been elected.
  bool elected;

  // Group members.
  set<UPID> members;

  ReplicaProcess* replica;
  GroupProcess* group;
  Coordinator* coordinator;
};


class LogProcessWatcher : public Watcher
{
public:
  LogProcessWatcher(const PID<LogProcess>& _pid)
    : pid(_pid), reconnect(false) {}

  virtual ~LogProcessWatcher() {}

  virtual void process(ZooKeeper* zk, int type, int state, const string& path)
  {
    if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_SESSION_EVENT)) {
      // Check if this is a reconnect.
      if (!reconnect) {
        // Initial connect.
        dispatch(pid, &LogProcess::connected);
      } else {
        // Reconnected.
        dispatch(pid, &LogProcess::reconnected);
      }
    } else if ((state == ZOO_CONNECTING_STATE) &&
               (type == ZOO_SESSION_EVENT)) {
      // The client library automatically reconnects, taking into
      // account failed servers in the connection string,
      // appropriately handling the "herd effect", etc.
      reconnect = true;
      dispatch(pid, &LogProcess::reconnecting);
    } else if ((state == ZOO_EXPIRED_SESSION_STATE) &&
               (type == ZOO_SESSION_EVENT)) {
      dispatch(pid, &LogProcess::expired);

      // If this watcher is reused, the next connect won't be a reconnect.
      reconnect = false;
    } else if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_CHILD_EVENT)) {
      dispatch(pid, &LogProcess::updated, path);
    } else if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_CHANGED_EVENT)) {
      dispatch(pid, &LogProcess::updated, path);
    } else {
      LOG(FATAL) << "Unimplemented ZooKeeper event: (state is "
                 << state << " and type is " << type << ")";
    }
  }

private:
  const PID<LogProcess> pid;
  bool reconnect;
};


LogProcess::LogProcess(int _quorum,
                       const string& _file,
                       const string& _servers,
                       const string& _znode,
                       int _end,
                       const map<int, int>& _truncations)
  : quorum(_quorum),
    file(_file),
    servers(_servers),
    znode(_znode),
    end(_end),
    truncations(_truncations),
    id(0),
    elected(false),
    replica(NULL),
    group(NULL),
    coordinator(NULL) {}


LogProcess::~LogProcess()
{
  delete zk;
  delete watcher;
  delete replica;
  delete group;
  delete coordinator;
}


void LogProcess::connected()
{
  LOG(INFO) << "Log connected to ZooKeeper";

  int ret;
  string result;

  // Assume the znode that was created does not end with a "/".
  CHECK(znode.size() == 0 || znode.at(znode.size() - 1) != '/');

  // Create directory path znodes as necessary.
  size_t index = znode.find("/", 0);

  while (index < string::npos) {
    // Get out the prefix to create.
    index = znode.find("/", index + 1);
    string prefix = znode.substr(0, index);

    LOG(INFO) << "Log trying to create znode '"
              << prefix << "' in ZooKeeper";

    // Create the node (even if it already exists).
    ret = zk->create(
        prefix,
        "",
        ZOO_OPEN_ACL_UNSAFE,
        // ZOO_CREATOR_ALL_ACL, // needs authentication
        0,
        &result);

    if (ret != ZOK && ret != ZNODEEXISTS) {
      LOG(FATAL) << "Failed to create '" << prefix
                 << "' in ZooKeeper: " << zk->message(ret);
    }
  }

  // Now create the "replicas" znode.
  LOG(INFO) << "Log trying to create znode '" << znode
            << "/replicas" << "' in ZooKeeper";

  // Create the node (even if it already exists).
  ret = zk->create(znode + "/replicas", "", ZOO_OPEN_ACL_UNSAFE,
                   // ZOO_CREATOR_ALL_ACL, // needs authentication
                   0, &result);

  if (ret != ZOK && ret != ZNODEEXISTS) {
    LOG(FATAL) << "Failed to create '" << znode << "/replicas"
               << "' in ZooKeeper: " << zk->message(ret);
  }

  // Now create the "coordinators" znode.
  LOG(INFO) << "Log trying to create znode '" << znode
            << "/coordinators" << "' in ZooKeeper";

  // Create the node (even if it already exists).
  ret = zk->create(znode + "/coordinators", "", ZOO_OPEN_ACL_UNSAFE,
                   // ZOO_CREATOR_ALL_ACL, // needs authentication
                   0, &result);

  if (ret != ZOK && ret != ZNODEEXISTS) {
    LOG(FATAL) << "Failed to create '" << znode << "/coordinators"
               << "' in ZooKeeper: " << zk->message(ret);
  }

  // Okay, create our replica, group, and coordinator.
  replica = new ReplicaProcess(file);
  spawn(replica);

  group = new GroupProcess();
  spawn(group);

  coordinator = new Coordinator(quorum, replica, group);

  // Set a watch on the replicas.
  ret = zk->getChildren(znode + "/replicas", true, NULL);

  if (ret != ZOK) {
    LOG(FATAL) << "Failed to set a watch on '" << znode << "/replicas"
               << "' in ZooKeeper: " << zk->message(ret);
  }

  // Set a watch on the coordinators.
  ret = zk->getChildren(znode + "/coordinators", true, NULL);

  if (ret != ZOK) {
    LOG(FATAL) << "Failed to set a watch on '" << znode << "/replicas"
               << "' in ZooKeeper: " << zk->message(ret);
  }

  // Add an ephemeral znode for our replica and coordinator.
  ret = zk->create(znode + "/replicas/", replica->self(), ZOO_OPEN_ACL_UNSAFE,
                   // ZOO_CREATOR_ALL_ACL, // needs authentication
                   ZOO_SEQUENCE | ZOO_EPHEMERAL, &result);

  if (ret != ZOK) {
    LOG(FATAL) << "Failed to create an ephmeral node at '" << znode
               << "/replica/" << "' in ZooKeeper: " << zk->message(ret);
  }

  ret = zk->create(znode + "/coordinators/", "", ZOO_OPEN_ACL_UNSAFE,
                   // ZOO_CREATOR_ALL_ACL, // needs authentication
                   ZOO_SEQUENCE | ZOO_EPHEMERAL, &result);

  if (ret != ZOK) {
    LOG(FATAL) << "Failed to create an ephmeral node at '" << znode
               << "/replica/" << "' in ZooKeeper: " << zk->message(ret);
  }

  // Save the sequence id but only grab the basename, e.g.,
  // "/path/to/znode/000000131" => "000000131".
  result = utils::os::basename(result);

  try {
    id = boost::lexical_cast<uint64_t>(result);
  } catch (boost::bad_lexical_cast&) {
    LOG(FATAL) << "Failed to convert '" << result << "' into an integer";
  }

  // Run an election!
  elect();
}


void LogProcess::reconnecting()
{
  LOG(INFO) << "Reconnecting to ZooKeeper";
}


void LogProcess::reconnected()
{
  LOG(INFO) << "Reconnected to ZooKeeper";
}


void LogProcess::expired()
{
  restart();
}


void LogProcess::updated(const string& path)
{
  if (znode + "/replicas" == path) {

    regroup();

    // Reset a watch on the replicas.
    int ret = zk->getChildren(znode + "/replicas", true, NULL);

    if (ret != ZOK) {
      LOG(FATAL) << "Failed to set a watch on '" << znode << "/replicas"
                 << "' in ZooKeeper: " << zk->message(ret);
    }
  } else {
    CHECK(znode + "/coordinators" == path);

    elect();

    // Reset a watch on the coordinators.
    int ret = zk->getChildren(znode + "/coordinators", true, NULL);

    if (ret != ZOK) {
      LOG(FATAL) << "Failed to set a watch on '" << znode << "/replicas"
                 << "' in ZooKeeper: " << zk->message(ret);
    }
  }
}


void LogProcess::initalize()
{
  // TODO(benh): Real testing requires injecting a ZooKeeper instance.
  watcher = new LogProcessWatcher(self());
  zk = new ZooKeeper(servers, 10000, watcher);
}


void LogProcess::regroup()
{
  vector<string> results;

  int ret = zk->getChildren(znode + "/replicas", false, &results);

  if (ret != ZOK) {
    LOG(FATAL) << "Failed to get children of '" << znode << "/replicas"
               << "' in ZooKeeper: " << zk->message(ret);
  }

  set<UPID> current;
  set<UPID> added;
  set<UPID> removed;

  foreach (const string& result, results) {
    string s;
    int ret = zk->get(znode + "/replicas/" + result, false, &s, NULL);
    UPID pid = s;
    current.insert(pid);
  }

  foreach (const UPID& pid, current) {
    if (members.count(pid) == 0) {
      added.insert(pid);
    }
  }

  foreach (const UPID& pid, members) {
    if (current.count(pid) == 0) {
      removed.insert(pid);
    }
  }

  foreach (const UPID& pid, added) {
    dispatch(group, &GroupProcess::add, pid);
    members.insert(pid);
  }

  foreach (const UPID& pid, removed) {
    dispatch(group, &GroupProcess::remove, pid);
    members.erase(pid);
  }
}


void LogProcess::elect()
{
  vector<string> results;

  int ret = zk->getChildren(znode + "/coordinators", false, &results);

  if (ret != ZOK) {
    LOG(FATAL) << "Failed to get children of '" << znode << "/coordinators"
               << "' in ZooKeeper: " << zk->message(ret);
  }

  // "Elect" the minimum ephemeral znode.
  uint64_t min = LONG_MAX;
  foreach (const string& result, results) {
    try {
      min = std::min(min, boost::lexical_cast<uint64_t>(result));
    } catch (boost::bad_lexical_cast&) {
      LOG(FATAL) << "Failed to convert '" << result << "' into an integer";
    }
  }

  if (id == min && !elected) {
    elected = true;
    process::run(&coordinate, coordinator, id, end, truncations);
  } else if (elected) {
    LOG(INFO) << "Restarting due to demoted";
    restart();
  }
}


int main(int argc, char** argv)
{
  if (argc < 6) {
    fatal("Usage: %s <quorum> <file> <servers> <znode> <end> <at> <to> ...",
          argv[0]);
  }

  args = argv;

  int quorum = atoi(argv[1]);
  string file = argv[2];
  string servers = argv[3];
  string znode = argv[4];
  int end = atoi(argv[5]);

  map<int, int> truncations;

  for (int i = 6; argv[i] != NULL; i += 2) {
    if (argv[i + 1] == NULL) {
      fatal("Expecting 'to' argument for truncation");
    }

    int at = atoi(argv[i]);
    int to = atoi(argv[i + 1]);

    truncations[at] = to;
  }

  process::initialize();

  LogProcess log(quorum, file, servers, znode, end, truncations);
  spawn(log);
  wait(log);

  return 0;
}
