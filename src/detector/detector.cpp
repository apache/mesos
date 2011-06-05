#include <unistd.h>

#include <process.hpp>

#include <iostream>
#include <climits>
#include <cstdlib>
#include <stdexcept>

#include <glog/logging.h>

#include <boost/lexical_cast.hpp>

#include "config/config.hpp"

#include "common/fatal.hpp"
#include "common/foreach.hpp"

#include "messaging/messages.hpp"

#include "detector.hpp"
#include "url_processor.hpp"

#ifdef WITH_ZOOKEEPER
#include "zookeeper.hpp"
#endif

using namespace mesos;
using namespace mesos::internal;

using namespace std;

using boost::lexical_cast;


#ifdef WITH_ZOOKEEPER
class ZooKeeperMasterDetector : public MasterDetector, public Watcher
{
public:
  /**
   * Uses ZooKeeper for both detecting masters and contending to be a
   * master.
   *
   * @param server comma separated list of server host:port pairs
   *
   * @param znode top-level "ZooKeeper node" (directory) to use
   * @param pid libprocess pid to send messages/updates to (and to
   * use for contending to be a master)
   * @param contend true if should contend to be master (not needed
   * for slaves and frameworks)
   * @param quiet verbosity logging level for undelying ZooKeeper library
   */
  ZooKeeperMasterDetector(const std::string &servers,
			  const std::string &znode,
			  const PID &pid,
			  bool contend = false,
			  bool quiet = false);

  virtual ~ZooKeeperMasterDetector();

  /** 
   * ZooKeeper watcher callback.
   */
  virtual void process(ZooKeeper *zk, int type, int state,
		       const std::string &path);

  /**
   * @return unique id of the current master
   */
  virtual std::string getCurrentMasterId();

  /**
   * @return libprocess PID of the current master
   */
  virtual PID getCurrentMasterPID();

private:
  /**
   * @param s sequence id
   */
  void setId(const std::string &s);

  /**
   * @return current sequence id if contending to be a master
   */
  std::string getId();

  /**
   * Attempts to detect a master.
   */
  void detectMaster();

  /**
   * @param seq sequence id of a master
   * @return PID corresponding to a master
   */
  PID lookupMasterPID(const std::string &seq) const;

  std::string servers;
  std::string znode;
  PID pid;
  bool contend;
  bool reconnect;

  ZooKeeper *zk;

  // Our sequence string if contending to be a master.
  std::string mySeq;

  std::string currentMasterSeq;
  PID currentMasterPID;
};
#endif /* #ifdef WITH_ZOOKEEPER */


MasterDetector::~MasterDetector() {}


MasterDetector * MasterDetector::create(const std::string &url,
					const PID &pid,
					bool contend,
					bool quiet)
{
  if (url == "")
    if (contend)
      return new BasicMasterDetector(pid);
    else
      fatal("cannot use specified url to detect master");

  MasterDetector *detector = NULL;

  // Parse the url.
  pair<UrlProcessor::URLType, string> urlPair = UrlProcessor::process(url);

  switch (urlPair.first) {
    // ZooKeeper URL.
    case UrlProcessor::ZOO: {
#ifdef WITH_ZOOKEEPER
      // TODO(benh): Consider actually using the chroot feature of
      // ZooKeeper, rather than just using it's syntax.
      size_t index = urlPair.second.find("/");
      if (index == string::npos)
	fatal("expecting chroot path for ZooKeeper");
      const string &znode = urlPair.second.substr(index);
      if (znode == "/")
	fatal("expecting chroot path for ZooKeeper ('/' is not supported)");
      const string &servers = urlPair.second.substr(0, index);
      detector = new ZooKeeperMasterDetector(servers, znode, pid, contend, quiet);
#else
      fatal("ZooKeeper not supported in this build");
#endif /* #ifdef WITH_ZOOKEEPER */
      break;
    }

    // Mesos URL or libprocess pid.
    case UrlProcessor::MESOS:
    case UrlProcessor::UNKNOWN: {
      if (contend) {
	// TODO(benh): Wierdnesses like this makes it seem like there
	// should be a separate elector and detector. In particular,
	// it doesn't make sense to pass a libprocess pid and attempt
	// to contend (at least not right now).
	fatal("cannot contend to be a master with specified url");
      } else {
	PID master(urlPair.second);
	if (!master)
	  fatal("cannot use specified url to detect master");
	detector = new BasicMasterDetector(master, pid);
      }
      break;
    }
  }

  return detector;
}


void MasterDetector::destroy(MasterDetector *detector)
{
  if (detector != NULL)
    delete detector;
}


BasicMasterDetector::BasicMasterDetector(const PID &_master)
  : master(_master)
{
  // Send a master id.
  MesosProcess::post(master, pack<GOT_MASTER_ID>("0"));

  // Elect the master.
  MesosProcess::post(master, pack<NEW_MASTER_DETECTED>("0", master));
}


BasicMasterDetector::BasicMasterDetector(const PID &_master,
					 const PID &pid,
					 bool elect)
  : master(_master)
{
  if (elect) {
    // Send a master id.
    MesosProcess::post(master, pack<GOT_MASTER_ID>("0"));

    // Elect the master.
    MesosProcess::post(master, pack<NEW_MASTER_DETECTED>("0", master));
  }

  // Tell the pid about the master.
  MesosProcess::post(pid, pack<NEW_MASTER_DETECTED>("0", master));
}


BasicMasterDetector::BasicMasterDetector(const PID &_master,
					 const vector<PID> &pids,
					 bool elect)
  : master(_master)
{
  if (elect) {
    // Send a master id.
    MesosProcess::post(master, pack<GOT_MASTER_ID>("0"));

    // Elect the master.
    MesosProcess::post(master, pack<NEW_MASTER_DETECTED>("0", master));
  }

  // Tell each pid about the master.
  foreach (const PID &pid, pids)
    MesosProcess::post(pid, pack<NEW_MASTER_DETECTED>("0", master));
}


BasicMasterDetector::~BasicMasterDetector() {}


string BasicMasterDetector::getCurrentMasterId()
{
  return "0";
}


PID BasicMasterDetector::getCurrentMasterPID()
{
  return master;
}


#ifdef WITH_ZOOKEEPER
ZooKeeperMasterDetector::ZooKeeperMasterDetector(const string &_servers,
						 const string &_znode,
						 const PID &_pid,
						 bool _contend,
						 bool quiet)
  : servers(_servers), znode(_znode), pid(_pid),
    contend(_contend), reconnect(false)
{
  // Set verbosity level for underlying ZooKeeper library logging.
  // TODO(benh): Put this in the C++ API.
  zoo_set_debug_level(quiet ? ZOO_LOG_LEVEL_ERROR : ZOO_LOG_LEVEL_DEBUG);

  // Start up the ZooKeeper connection!
  zk = new ZooKeeper(servers, 10000, this);
}


ZooKeeperMasterDetector::~ZooKeeperMasterDetector()
{
  if (zk != NULL) {
    delete zk;
    zk = NULL;
  }
}


void ZooKeeperMasterDetector::process(ZooKeeper *zk, int type, int state,
				      const string &path)
{
  int ret;
  string result;

  static const string delimiter = "/";

  if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_SESSION_EVENT)) {
    // Check if this is a reconnect.
    if (!reconnect) {
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
	// We contend with the pid given in constructor.
	ret = zk->create(znode + "/", pid, ZOO_CREATOR_ALL_ACL,
			 ZOO_SEQUENCE | ZOO_EPHEMERAL, &result);

	if (ret != ZOK)
	  fatal("ZooKeeper not responding correctly (%s). "
		"Make sure ZooKeeper is running on: %s",
		zk->error(ret), servers.c_str());

	setId(result);
	LOG(INFO) << "Created ephemeral/sequence:" << getId();

        MesosProcess::post(pid, pack<GOT_MASTER_ID>(getId()));
      }

      // Now determine who the master is (it may be us).
      detectMaster();
    } else {
      // Reconnected.
      if (contend) {
	// Contending for master, confirm our ephemeral sequence znode exists.
	ret = zk->get(znode + "/" + mySeq, false, &result, NULL);

	// We might no longer be the master! Commit suicide for now
	// (hoping another master is on standbye), but in the future
	// it would be nice if we could go back on standbye.
	if (ret == ZNONODE)
	  fatal("failed to reconnect to ZooKeeper quickly enough "
		"(our ephemeral sequence znode is gone), commiting suicide!");

	if (ret != ZOK)
	  fatal("ZooKeeper not responding correctly (%s). "
		"Make sure ZooKeeper is running on: %s",
		zk->error(ret), servers.c_str());

	// We are still the master!
	LOG(INFO) << "Reconnected to Zookeeper, still acting as master.";
      } else {
	// Reconnected, but maybe the master changed?
	detectMaster();
      }

      reconnect = false;
    }
  } else if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_CHILD_EVENT)) {
    // A new master might have showed up and created a sequence
    // identifier or a master may have died, determine who the master is now!
    detectMaster();
  } else if ((state == ZOO_CONNECTING_STATE) && (type == ZOO_SESSION_EVENT)) {
    // The client library automatically reconnects, taking into
    // account failed servers in the connection string,
    // appropriately handling the "herd effect", etc.
    LOG(INFO) << "Lost Zookeeper connection. Retrying (automagically).";
    reconnect = true;
  } else {
    LOG(INFO) << "Unimplemented watch event: (state is "
	      << state << " and type is " << type << ")";
  }
}


void ZooKeeperMasterDetector::setId(const string &s)
{
  string seq = s;
  // Converts "/path/to/znode/000000131" to "000000131".
  int pos;
  if ((pos = seq.find_last_of('/')) != string::npos) {  
    mySeq = seq.erase(0, pos + 1);
  } else
    mySeq = "";
}


string ZooKeeperMasterDetector::getId() 
{
  return mySeq;
}


void ZooKeeperMasterDetector::detectMaster()
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
    MesosProcess::post(pid, pack<NO_MASTER_DETECTED>());
  } else if (masterSeq != currentMasterSeq) {
    currentMasterSeq = masterSeq;
    currentMasterPID = lookupMasterPID(masterSeq); 

    // While trying to get the master PID, master might have crashed,
    // so PID might be empty.
    if (currentMasterPID == PID())
      MesosProcess::post(pid, pack<NO_MASTER_DETECTED>());
    else
      MesosProcess::post(pid, pack<NEW_MASTER_DETECTED>(currentMasterSeq,
                                                        currentMasterPID));
  }
}


PID ZooKeeperMasterDetector::lookupMasterPID(const string &seq) const
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
  return PID(result);
}


string ZooKeeperMasterDetector::getCurrentMasterId()
{
  return currentMasterSeq;
}


PID ZooKeeperMasterDetector::getCurrentMasterPID()
{
  return currentMasterPID;
}
#endif /* #ifdef WITH_ZOOKEEPER */
