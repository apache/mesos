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

#include <glog/logging.h>

#include <fstream>
#include <ios>
#include <vector>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/timer.hpp>

#include <stout/foreach.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "detector/detector.hpp"

#include "logging/logging.hpp"

#include "messages/messages.hpp"

#include "zookeeper/authentication.hpp"
#include "zookeeper/url.hpp"
#include "zookeeper/watcher.hpp"
#include "zookeeper/zookeeper.hpp"

using process::Future;
using process::Process;
using process::Timer;
using process::UPID;
using process::wait; // Necessary on some OS's to disambiguate.

using std::pair;
using std::string;
using std::vector;

namespace mesos {
namespace internal {

// TODO(benh): Make this value configurable via flags and verify that
// it is always LESS THAN the slave heartbeat timeout.
const Seconds ZOOKEEPER_SESSION_TIMEOUT(10.0);


class ZooKeeperMasterDetectorProcess
  : public Process<ZooKeeperMasterDetectorProcess>
{
public:
  ZooKeeperMasterDetectorProcess(
    const zookeeper::URL& url,
    const UPID& pid,
    bool contend,
    bool quiet);

  virtual ~ZooKeeperMasterDetectorProcess();

  virtual void initialize();

  // ZooKeeperMasterDetector implementation.
  int64_t session();

  // ZooKeeper events.
  void connected(bool reconnect);
  void reconnecting();
  void expired();
  void updated(const string& path);
  void created(const string& path);
  void deleted(const string& path);

private:
  // Handles reconnecting "timeouts" by prematurely expiring a session
  // (only used for contending instances). TODO(benh): Remove 'const
  // &' after fixing libprocess.
  void timedout(const int64_t& sessionId);

  // Attempts to detect a master.
  void detectMaster();

  const zookeeper::URL url;
  const ACL_vector acl;

  const UPID pid;
  bool contend;

  Watcher* watcher;
  ZooKeeper* zk;

  bool expire;
  Option<Timer> timer;

  string currentMasterSeq;
  UPID currentMasterPID;
};


MasterDetector::~MasterDetector() {}


Try<MasterDetector*> MasterDetector::create(const string& master,
                                            const UPID& pid,
                                            bool contend,
                                            bool quiet)
{
  if (master == "") {
    if (contend) {
      return new BasicMasterDetector(pid);
    } else {
      return Try<MasterDetector*>::error("Cannot detect master");
    }
  } else if (master.find("zk://") == 0) {
    Try<zookeeper::URL> url = zookeeper::URL::parse(master);
    if (url.isError()) {
      return Try<MasterDetector*>::error(url.error());
    }
    if (url.get().path == "/") {
      return Try<MasterDetector*>::error(
          "Expecting a (chroot) path for ZooKeeper ('/' is not supported)");
    }
    return new ZooKeeperMasterDetector(url.get(), pid, contend, quiet);
  } else if (master.find("file://") == 0) {
    const std::string& path = master.substr(7);
    std::ifstream file(path.c_str());
    if (!file.is_open()) {
      return Try<MasterDetector*>::error(
          "Failed to open file at '" + path + "'");
    }

    std::string line;
    getline(file, line);

    if (!file) {
      file.close();
      return Try<MasterDetector*>::error(
          "Failed to read from file at '" + path + "'");
    }

    file.close();

    return create(line, pid, contend, quiet);
  }

  // Okay, try and parse what we got as a PID.
  process::UPID masterPid = master.find("master@") == 0
    ? process::UPID(master)
    : process::UPID("master@" + master);

  if (!masterPid) {
    return Try<MasterDetector*>::error(
        "Cannot parse '" + std::string(masterPid) + "'");
  }

  return new BasicMasterDetector(masterPid, pid);
}


void MasterDetector::destroy(MasterDetector *detector)
{
  if (detector != NULL)
    delete detector;
}


BasicMasterDetector::BasicMasterDetector(const UPID& _master)
  : master(_master)
{
  // Elect the master.
  NewMasterDetectedMessage message;
  message.set_pid(master);
  process::post(master, message);
}


BasicMasterDetector::BasicMasterDetector(const UPID& _master,
					 const UPID& pid,
					 bool elect)
  : master(_master)
{
  if (elect) {
    // Elect the master.
    NewMasterDetectedMessage message;
    message.set_pid(master);
    process::post(master, message);
  }

  // Tell the pid about the master.
  NewMasterDetectedMessage message;
  message.set_pid(master);
  process::post(pid, message);
}


BasicMasterDetector::BasicMasterDetector(const UPID& _master,
					 const vector<UPID>& pids,
					 bool elect)
  : master(_master)
{
  if (elect) {
    // Elect the master.
    NewMasterDetectedMessage message;
    message.set_pid(master);
    process::post(master, message);
  }

  // Tell each pid about the master.
  foreach (const UPID& pid, pids) {
    NewMasterDetectedMessage message;
    message.set_pid(master);
    process::post(pid, message);
  }
}


BasicMasterDetector::~BasicMasterDetector() {}


ZooKeeperMasterDetectorProcess::ZooKeeperMasterDetectorProcess(
    const zookeeper::URL& _url,
    const UPID& _pid,
    bool _contend,
    bool quiet)
  : url(_url),
    acl(url.authentication.isSome()
        ? zookeeper::EVERYONE_READ_CREATOR_ALL
        : ZOO_OPEN_ACL_UNSAFE),
    pid(_pid),
    contend(_contend),
    watcher(NULL),
    zk(NULL),
    expire(false)
{
  // Set verbosity level for underlying ZooKeeper library logging.
  // TODO(benh): Put this in the C++ API.
  zoo_set_debug_level(quiet ? ZOO_LOG_LEVEL_ERROR : ZOO_LOG_LEVEL_DEBUG);
}


ZooKeeperMasterDetectorProcess::~ZooKeeperMasterDetectorProcess()
{
  delete zk;
  delete watcher;
}


void ZooKeeperMasterDetectorProcess::initialize()
{
  // Doing initialization here allows to avoid the race between
  // instantiating the ZooKeeper instance and being spawned ourself.
  watcher = new ProcessWatcher<ZooKeeperMasterDetectorProcess>(self());
  zk = new ZooKeeper(url.servers, ZOOKEEPER_SESSION_TIMEOUT, watcher);
}


int64_t ZooKeeperMasterDetectorProcess::session()
{
  CHECK_NOTNULL(zk);
  return zk->getSessionId();
}


void ZooKeeperMasterDetectorProcess::connected(bool reconnect)
{
  if (!reconnect) {
    LOG(INFO) << "Master detector connected to ZooKeeper ...";

    if (url.authentication.isSome()) {
      const std::string& scheme = url.authentication.get().scheme;
      const std::string& credentials = url.authentication.get().credentials;
      LOG(INFO) << "Authenticating to ZooKeeper using scheme '" << scheme << "'";
      int code = zk->authenticate(scheme, credentials);
      if (code != ZOK) {
        LOG(FATAL) << "Failed to authenticate with ZooKeeper: "
                   << zk->message(code);
      }
    }

    // Assume the path (chroot) being used does not end with a "/".
    CHECK(url.path.at(url.path.length() - 1) != '/');

    // Create znode path (including intermediate znodes) as necessary.
    LOG(INFO) << "Trying to create path '" << url.path << "' in ZooKeeper";

    int code = zk->create(url.path, "", acl, 0, NULL, true);

    // We fail all non-OK return codes except ZNODEEXISTS (since that
    // means the path we were trying to create exists) and ZNOAUTH
    // (since it's possible that the ACLs on 'dirname(url.path)' don't
    // allow us to create a child znode but we are allowed to create
    // children of 'url.path' itself, which will be determined below
    // if we are contending). Note that it's also possible we got back
    // a ZNONODE because we could not create one of the intermediate
    // znodes (in which case we'll abort in the 'else' below since
    // ZNONODE is non-retryable). TODO(benh): Need to check that we
    // also can put a watch on the children of 'url.path'.
    if (code != ZOK && code != ZNODEEXISTS && code != ZNOAUTH) {
      LOG(FATAL) << "Failed to create '" << url.path
                 << "' in ZooKeeper: " << zk->message(code);
    }

    if (contend) {
      // We contend with the pid given in constructor.
      string result;
      int code = zk->create(url.path + "/", pid, acl,
                            ZOO_SEQUENCE | ZOO_EPHEMERAL, &result);

      if (code != ZOK) {
        LOG(FATAL) << "Unable to create ephemeral child of '" << url.path
                   << "' in ZooKeeper: %s" << zk->message(code);
      }

      LOG(INFO) << "Created ephemeral/sequence znode at '" << result << "'";
    }

    // Now determine who the master is (it may be us).
    detectMaster();
  } else {
    LOG(INFO) << "Master detector reconnected ...";

    // Cancel and cleanup the reconnect timer (if necessary).
    if (timer.isSome()) {
      Timer::cancel(timer.get());
      timer = Option<Timer>::none();
    }

    // If we decided to expire the session, make sure we delete the
    // ZooKeeper instance so the session actually expires. We also
    // create a new ZooKeeper instance for clients that want to
    // continue detecting and/or contending (which is likely given
    // that this code is getting executed).
    if (expire) {
      LOG(WARNING) << "Cleaning up after expired ZooKeeper session";

      delete CHECK_NOTNULL(zk);
      delete CHECK_NOTNULL(watcher);

      watcher = new ProcessWatcher<ZooKeeperMasterDetectorProcess>(self());
      zk = new ZooKeeper(url.servers, ZOOKEEPER_SESSION_TIMEOUT, watcher);

      expire = false;
      return;
    }

    // We've reconnected and we didn't prematurely expire the session,
    // but the master might have changed, so we should run an
    // election. TODO(benh): Determine if this is really necessary or
    // if the watch set via 'ZooKeeper::getChildren' in 'detectMaster'
    // is sufficient (it should be).
    detectMaster();
  }
}


void ZooKeeperMasterDetectorProcess::reconnecting()
{
  LOG(INFO) << "Master detector lost connection to ZooKeeper, "
            << "attempting to reconnect ...";

  // ZooKeeper won't tell us of a session expiration until we
  // reconnect, which could occur much much later than the session was
  // actually expired. This can lead to a prolonged split-brain
  // scenario when network partitions occur. Rather than wait for a
  // reconnection to occur (i.e., a network partition to be repaired)
  // we create a local timer and "expire" our session prematurely if
  // we haven't reconnected within the session expiration time
  // out. Later, when we eventually do reconnect we can force the
  // session to be expired if we decided locally to expire.
  timer = process::delay(
      ZOOKEEPER_SESSION_TIMEOUT, self(), &Self::timedout, zk->getSessionId());
}


void ZooKeeperMasterDetectorProcess::expired()
{
  LOG(WARNING) << "Master detector ZooKeeper session expired!";

  // Cancel and cleanup the reconnect timer (if necessary).
  if (timer.isSome()) {
    Timer::cancel(timer.get());
    timer = Option<Timer>::none();
  }

  delete CHECK_NOTNULL(zk);
  delete CHECK_NOTNULL(watcher);

  watcher = new ProcessWatcher<ZooKeeperMasterDetectorProcess>(self());
  zk = new ZooKeeper(url.servers, ZOOKEEPER_SESSION_TIMEOUT, watcher);

  expire = false;
}


void ZooKeeperMasterDetectorProcess::updated(const string& path)
{
  // A new master might have showed up and created a sequence
  // identifier or a master may have died, determine who the master is now!
  detectMaster();
}


void ZooKeeperMasterDetectorProcess::created(const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event (created) for '" << path << "'";
}


void ZooKeeperMasterDetectorProcess::deleted(const string& path)
{
  LOG(FATAL) << "Unexpected ZooKeeper event (deleted) for '" << path << "'";
}


void ZooKeeperMasterDetectorProcess::timedout(const int64_t& sessionId)
{
  CHECK_NOTNULL(zk);
  if (timer.isSome() && zk->getSessionId() == sessionId) {
    LOG(WARNING) << "Timed out waiting to reconnect to ZooKeeper "
                 << "(sessionId=" << std::hex << sessionId << ")";
    timer = Option<Timer>::none();
    expire = true;

    // We only send a NoMasterDetectedMessage if we are a
    // contending detector AND ALSO the current leader.
    // This is because:
    // 1) If we are a non-contending detector (e.g. slave), a zk session
    //    expiration doesn't necessarily mean a new leader (master) is elected
    //    (e.g. the slave is partitioned from the zk server). If the leading
    //    master stays the same (i.e., no leader election), then the
    //    slave should still accept a ShutDownMessage from the master.
    //    If a new master does get elected, the slave would know about it
    //    because it would do a leader detection after it connects/re-connects.
    // 2) If we are a contender but not the leader (e.g. non-leading master),
    //    sending a NoMasterDetectedMessage() is bad because, a partitioned
    //    non-leading master would never know about the leading master that
    //    stays the same (i.e., no leader election) even after it is
    //    connected/reconnected with the ZooKeeper. This is because, the
    //    the master detection code (detectMaster()) will not send a
    //    NewMasterDetectedMessage to the non-leading master as there is no
    //    change in the currentMasterSeq.
    if (contend && currentMasterPID == pid) {
      process::post(pid, NoMasterDetectedMessage());
    }
  }
}


void ZooKeeperMasterDetectorProcess::detectMaster()
{
  vector<string> results;

  int code = zk->getChildren(url.path, true, &results);

  if (code != ZOK) {
    if (zk->retryable(code)) {
      // NOTE: We don't expect a ZNONODE here because 'url.path' is always
      // created in the connected() call. Despite that, we don't do a
      // CHECK (code != ZNONODE) just to be safe incase the zk client library
      // does return the code unexpectedly.
      LOG(ERROR) << "Master detector failed to get masters: "
                 << zk->message(code);
      return; // Try again when we reconnect.
    } else {
      LOG(FATAL) << "Non-retryable ZooKeeper error while getting masters: "
                 << zk->message(code);
    }
  } else {
    LOG(INFO) << "Master detector found " << results.size()
              << " registered masters";
  }

  string masterSeq;
  long min = LONG_MAX;
  foreach (const string& result, results) {
    Try<int> i = numify<int>(result);
    if (i.isError()) {
      LOG(WARNING) << "Unexpected znode at '" << url.path
                   << "': " << i.error();
      continue;
    }
    if (i.get() < min) {
      min = i.get();
      masterSeq = result;
    }
  }

  // No master present (lost or possibly hasn't come up yet).
  if (masterSeq.empty()) {
    LOG(INFO) << "Master detector of " << pid << " couldn't find any masters";
    process::post(pid, NoMasterDetectedMessage());
  } else if (masterSeq != currentMasterSeq) {
    // Okay, let's fetch the master pid from ZooKeeper.
    string result;
    code = zk->get(url.path + "/" + masterSeq, false, &result, NULL);

    if (code != ZOK) {
      // This is possible because the master might have failed since
      // the invocation of ZooKeeper::getChildren above.
      // It is fine to not send a NoMasterDetectedMessage here because,
      // 1) If this is due to a connection loss or session expiration,
      //    connected() or expired() will be called and the leader detection
      //    code (detectMaster()) will be re-tried.
      // 2) If this is due to no masters present (i.e., code == ZNONODE),
      //    updated() will be called and the detectMaster() will be re-tried.
      if (zk->retryable(code)) {
        LOG(ERROR) << "Master detector failed to fetch new master pid: "
                   << zk->message(code);
      } else {
        LOG(FATAL) << "Non-retryable ZooKeeper error while fetching "
                   << "new master pid: " << zk->message(code);
      }
    } else {
      // Now let's parse what we fetched from ZooKeeper.
      LOG(INFO) << "Master detector got new master pid: " << result;

      UPID masterPid = result;

      if (masterPid == UPID()) {
        // TODO(benh): Maybe we should try again then!?!? Parsing
        // might have failed because of DNS, and whoever is using the
        // detector might sit "unconnected" indefinitely!
        LOG(ERROR) << "Failed to parse new master pid!";
        process::post(pid, NoMasterDetectedMessage());
      } else {
        currentMasterSeq = masterSeq;
        currentMasterPID = masterPid;

        NewMasterDetectedMessage message;
        message.set_pid(currentMasterPID);
        process::post(pid, message);
      }
    }
  }
}


ZooKeeperMasterDetector::ZooKeeperMasterDetector(
    const zookeeper::URL& url,
    const UPID& pid,
    bool contend,
    bool quiet)
{
  process = new ZooKeeperMasterDetectorProcess(url, pid, contend, quiet);
  spawn(process);
}


ZooKeeperMasterDetector::~ZooKeeperMasterDetector()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<int64_t> ZooKeeperMasterDetector::session()
{
  return dispatch(process, &ZooKeeperMasterDetectorProcess::session);
}

} // namespace internal {
} // namespace mesos {
