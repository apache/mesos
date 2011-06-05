#include <unistd.h>

#include <process.hpp>

#include <iostream>
#include <climits>
#include <cstdlib>
#include <stdexcept>

#include <glog/logging.h>

#include <boost/lexical_cast.hpp>

#include "fatal.hpp"
#include "master_detector.hpp"
#include "messages.hpp"

using namespace nexus;
using namespace nexus::internal;

using boost::lexical_cast;


MasterDetector::MasterDetector(const string &_servers, const string &_znode,
			       const PID &_pid, bool _contend)
  : servers(_servers), znode(_znode), pid(_pid), contend(_contend)
{
  zk = new ZooKeeper(servers, 10000, this);
}


MasterDetector::~MasterDetector()
{
  if (zk != NULL)
    delete zk;
}


void MasterDetector::process(ZooKeeper *zk, int type, int state,
			     const string &path)
{
  int ret;
  string result;

  static const string delimiter = "/";

  if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_SESSION_EVENT)) {
    // Assume the znode that was created does not end with a "/".
    CHECK(znode.at(znode.length() - 1) != '/');

    // Create directory path znodes as necessary.
    size_t index = znode.find(delimiter, 0);

    while (index < string::npos) {
      // Get out the prefix to create.
      index = znode.find(delimiter, index + 1);
      string prefix = znode.substr(0, index);

      // Create the node (even if it already exists).
      ret = zk->create(prefix, "", ZOO_CREATOR_ALL_ACL,
		       0, &result);

      if (ret != ZOK && ret != ZNODEEXISTS)
	fatal("failed to create ZooKeeper znode! (%s)", zk->error(ret));
    }

    // Wierdness in ZooKeeper timing, let's check that everything is created.
    ret = zk->get(znode, false, &result, NULL);

    if (ret != ZOK)
      fatal("ZooKeeper not responding correctly (%s). "
	    "Make sure ZooKeeper is running on: %s",
	    zk->error(ret), servers.c_str());

    if (contend) {
      // We use the contend with the pid given in constructor.
      ret = zk->create(znode + "/", pid, ZOO_CREATOR_ALL_ACL,
		       ZOO_SEQUENCE | ZOO_EPHEMERAL, &result);

      if (ret != ZOK)
	fatal("ZooKeeper not responding correctly (%s). "
	      "Make sure ZooKeeper is running on: %s",
	      zk->error(ret), servers.c_str());

      setMySeq(result);
      LOG(INFO) << "Created ephemeral/sequence:" << getMySeq();

      const string &s =
	Tuple<Process>::tupleToString(Tuple<Process>::pack<GOT_MASTER_SEQ>(getMySeq()));
      Process::post(pid, GOT_MASTER_SEQ, s.data(), s.size());
    }

    // Now determine who the master is (it may be us).
    detectMaster();
  } else if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_CHILD_EVENT)) {
    // A new master might have showed up and created a sequence
    // identifier or a master may have died, determine who the master is now!
    detectMaster();
  } else {
    LOG(INFO) << "Unimplemented watch event: (state is "
	      << state << " and type is " << type << ")";
  }
}


void MasterDetector::detectMaster()
{
  vector<string> results;

  int ret = zk->getChildren(znode, true, &results);

  if (ret != ZOK)
    LOG(ERROR) << "failed to get masters: " << zk->error(ret);
  else
    LOG(INFO) << "found " << results.size() << " registered masters";

  string masterSeq;
  long min = LONG_MAX;
  foreach (const string &result, results) {
    int i = lexical_cast<int>(result);
    if (i < min) {
      min = i;
      masterSeq = result;
    }
  }

  // No master present (lost or possibly hasn't come up yet).
  if (masterSeq.empty()) {
    const string &s =
      Tuple<Process>::tupleToString(Tuple<Process>::pack<NO_MASTER_DETECTED>());
    Process::post(pid, NO_MASTER_DETECTED, s.data(), s.size());
  } else if (masterSeq != currentMasterSeq) {
    currentMasterSeq = masterSeq;
    currentMasterPID = lookupMasterPID(masterSeq); 

    // While trying to get the master PID, master might have crashed,
    // so PID might be empty.
    if (currentMasterPID == PID()) {
      const string &s =
	Tuple<Process>::tupleToString(Tuple<Process>::pack<NO_MASTER_DETECTED>());
      Process::post(pid, NO_MASTER_DETECTED, s.data(), s.size());
    } else {
      const string &s =
	Tuple<Process>::tupleToString(Tuple<Process>::pack<NEW_MASTER_DETECTED>(currentMasterSeq, currentMasterPID));
      Process::post(pid, NEW_MASTER_DETECTED, s.data(), s.size());
    }
  }
}


PID MasterDetector::lookupMasterPID(const string &seq) const
{
  CHECK(!seq.empty());

  int ret;
  string result;

  ret = zk->get(znode + "/" + seq, false, &result, NULL);

  if (ret != ZOK)
    LOG(ERROR) << "failed to fetch new master pid: " << zk->error(ret);
  else
    LOG(INFO) << "got new master pid: " << result;

  // TODO(benh): Automatic cast!
  return make_pid(result.c_str());
}


string MasterDetector::getCurrentMasterSeq() const {
  return currentMasterSeq;
}


PID MasterDetector::getCurrentMasterPID() const {
  return currentMasterPID;
}
