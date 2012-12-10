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
#include <vector>

#include <boost/lexical_cast.hpp>

#include <process/protobuf.hpp>

#include <stout/foreach.hpp>

#include "detector/detector.hpp"

#include "logging/logging.hpp"

#include "messages/messages.hpp"

#include "zookeeper/authentication.hpp"
#include "zookeeper/url.hpp"
#include "zookeeper/zookeeper.hpp"

using namespace mesos;
using namespace mesos::internal;

using boost::lexical_cast;

using process::Process;
using process::UPID;

using std::pair;
using std::string;
using std::vector;


class ZooKeeperMasterDetector : public MasterDetector, public Watcher
{
public:
  /**
   * Uses ZooKeeper for both detecting masters and contending to be a
   * master.
   *
   * @param server comma separated list of server host:port pairs
   * @param znode top-level "ZooKeeper node" (directory) to use
   * @param pid libprocess pid to send messages/updates to (and to
   * use for contending to be a master)
   * @param contend true if should contend to be master and false otherwise (not
   * needed for slaves and frameworks)
   * @param quiet verbosity logging level for underlying ZooKeeper library
   */
  ZooKeeperMasterDetector(const zookeeper::URL& url,
                          const UPID& pid,
                          bool contend,
                          bool quiet);

  virtual ~ZooKeeperMasterDetector();

  /**
   * ZooKeeper watcher callback.
   */
  virtual void process(ZooKeeper *zk, int type, int state, const string &path);

private:
  void connected();
  void reconnecting();
  void reconnected();
  void expired();
  void updated(const string& path);

  /**
   * Attempts to detect a master.
   */
  void detectMaster();

  const zookeeper::URL url;
  const UPID pid;
  bool contend;

  bool reconnect;

  ACL_vector acl;

  ZooKeeper* zk;

  // Our sequence string if contending to be a master.
  string mySeq;

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
  // Send a master token.
  {
    GotMasterTokenMessage message;
    message.set_token("0");
    process::post(master, message);
  }

  // Elect the master.
  {
    NewMasterDetectedMessage message;
    message.set_pid(master);
    process::post(master, message);
  }
}


BasicMasterDetector::BasicMasterDetector(const UPID& _master,
					 const UPID& pid,
					 bool elect)
  : master(_master)
{
  if (elect) {
    // Send a master token.
    {
      GotMasterTokenMessage message;
      message.set_token("0");
      process::post(master, message);
    }

    // Elect the master.
    {
      NewMasterDetectedMessage message;
      message.set_pid(master);
      process::post(master, message);
    }
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
    // Send a master token.
    {
      GotMasterTokenMessage message;
      message.set_token("0");
      process::post(master, message);
    }

    // Elect the master.
    {
      NewMasterDetectedMessage message;
      message.set_pid(master);
      process::post(master, message);
    }
  }

  // Tell each pid about the master.
  foreach (const UPID& pid, pids) {
    NewMasterDetectedMessage message;
    message.set_pid(master);
    process::post(pid, message);
  }
}


BasicMasterDetector::~BasicMasterDetector() {}


ZooKeeperMasterDetector::ZooKeeperMasterDetector(
    const zookeeper::URL& _url,
    const UPID& _pid,
    bool _contend,
    bool quiet)
  : url(_url),
    pid(_pid),
    contend(_contend),
    reconnect(false)
{
  // Set verbosity level for underlying ZooKeeper library logging.
  // TODO(benh): Put this in the C++ API.
  zoo_set_debug_level(quiet ? ZOO_LOG_LEVEL_ERROR : ZOO_LOG_LEVEL_DEBUG);

  acl = url.authentication.isSome()
    ? zookeeper::EVERYONE_READ_CREATOR_ALL
    : ZOO_OPEN_ACL_UNSAFE;

  // Start up the ZooKeeper connection!
  zk = new ZooKeeper(url.servers, milliseconds(10000), this);
}

ZooKeeperMasterDetector::~ZooKeeperMasterDetector()
{
  delete zk;
}


void ZooKeeperMasterDetector::connected()
{
  LOG(INFO) << "Master detector connected to ZooKeeper ...";

  int code;
  if (url.authentication.isSome()) {
    const std::string& scheme = url.authentication.get().scheme;
    const std::string& credentials = url.authentication.get().credentials;
    LOG(INFO) << "Authenticating to ZooKeeper using scheme '" << scheme << "'";
    code = zk->authenticate(scheme, credentials);
    if (code != ZOK) {
      LOG(FATAL) << "Failed to authenticate with ZooKeeper: "
                 << zk->message(code);
    }
  }

  string result;

  static const string delimiter = "/";

  // Assume the path (chroot) being used does not end with a "/".
  CHECK(url.path.at(url.path.length() - 1) != '/');

  // Create znodes as necessary.
  size_t index = url.path.find(delimiter, 0);

  while (index < string::npos) {
    // Get out the prefix to create.
    index = url.path.find(delimiter, index + 1);
    string prefix = url.path.substr(0, index);

    LOG(INFO) << "Trying to create znode '" << prefix << "' in ZooKeeper";

    // Create the node (even if it already exists).
    code = zk->create(prefix, "", acl, 0, &result);

    if (code != ZOK && code != ZNODEEXISTS) {
      LOG(FATAL) << "Failed to create ZooKeeper znode: " << zk->message(code);
    }
  }

  // Wierdness in ZooKeeper timing, let's check that everything is created.
  code = zk->get(url.path, false, &result, NULL);

  if (code != ZOK) {
    LOG(FATAL) << "Unexpected ZooKeeper failure: " << zk->message(code);
  }

  if (contend) {
    // We contend with the pid given in constructor.
    code = zk->create(url.path + "/", pid, acl,
                     ZOO_SEQUENCE | ZOO_EPHEMERAL, &result);

    if (code != ZOK) {
      LOG(FATAL) << "Unexpected ZooKeeper failure: %s" << zk->message(code);
    }

    // Save the sequence id but only grab the basename, e.g.,
    // "/path/to/znode/000000131" => "000000131".
    size_t index;
    if ((index = result.find_last_of('/')) != string::npos) {
      mySeq = result.erase(0, index + 1);
    } else {
      mySeq = result;
    }

    LOG(INFO) << "Created ephemeral/sequence:" << mySeq;

    GotMasterTokenMessage message;
    message.set_token(mySeq);
    process::post(pid, message);
  }

  // Now determine who the master is (it may be us).
  detectMaster();
}


void ZooKeeperMasterDetector::reconnecting()
{
  LOG(INFO) << "Master detector lost connection to ZooKeeper, "
	    << "attempting to reconnect ...";
}


void ZooKeeperMasterDetector::reconnected()
{
  LOG(INFO) << "Master detector reconnected ...";

  // Either we were the master and we're still the master (because we
  // haven't yet gotten a session expiration), or someone else was the
  // master and they're still the master, or someone else was the
  // master and someone else still is now the master. Either way, run
  // the leader detector.
  detectMaster();
}


void ZooKeeperMasterDetector::expired()
{
  LOG(WARNING) << "Master detector ZooKeeper session expired!";

  CHECK(zk != NULL);
  delete zk;

  zk = new ZooKeeper(url.servers, milliseconds(10000), this);
}


void ZooKeeperMasterDetector::updated(const string& path)
{
  // A new master might have showed up and created a sequence
  // identifier or a master may have died, determine who the master is now!
  detectMaster();
}


void ZooKeeperMasterDetector::process(ZooKeeper* zk, int type, int state,
				      const string& path)
{
  if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_SESSION_EVENT)) {
    // Check if this is a reconnect.
    if (!reconnect) {
      // Initial connect.
      connected();
    } else {
      // Reconnected.
      reconnected();
    }
  } else if ((state == ZOO_CONNECTING_STATE) && (type == ZOO_SESSION_EVENT)) {
    // The client library automatically reconnects, taking into
    // account failed servers in the connection string,
    // appropriately handling the "herd effect", etc.
    reconnect = true;
    reconnecting();
  } else if ((state == ZOO_EXPIRED_SESSION_STATE) && (type == ZOO_SESSION_EVENT)) {
    // Session expiration. Let the manager take care of it.
    expired();

    // If this watcher is reused, the next connect won't be a reconnect.
    reconnect = false;
  } else if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_CHILD_EVENT)) {
    updated(path);
  } else if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_CHANGED_EVENT)) {
    updated(path);
  } else {
    LOG(WARNING) << "Unimplemented watch event: (state is "
		 << state << " and type is " << type << ")";
  }
}


void ZooKeeperMasterDetector::detectMaster()
{
  vector<string> results;

  int code = zk->getChildren(url.path, true, &results);

  if (code != ZOK) {
    LOG(ERROR) << "Master detector failed to get masters: "
               << zk->message(code);
  } else {
    LOG(INFO) << "Master detector found " << results.size()
              << " registered masters";
  }

  string masterSeq;
  long min = LONG_MAX;
  foreach (const string& result, results) {
    int i = lexical_cast<int>(result);
    if (i < min) {
      min = i;
      masterSeq = result;
    }
  }

  // No master present (lost or possibly hasn't come up yet).
  if (masterSeq.empty()) {
    process::post(pid, NoMasterDetectedMessage());
  } else if (masterSeq != currentMasterSeq) {
    // Okay, let's fetch the master pid from ZooKeeper.
    string result;
    code = zk->get(url.path + "/" + masterSeq, false, &result, NULL);

    if (code != ZOK) {
      // This is possible because the master might have failed since
      // the invocation of ZooKeeper::getChildren above.
      LOG(ERROR) << "Master detector failed to fetch new master pid: "
		 << zk->message(code);
      process::post(pid, NoMasterDetectedMessage());
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
