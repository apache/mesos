#include <vector>

#include <glog/logging.h>

#include <boost/lexical_cast.hpp>

#include "config/config.hpp"

#include "common/fatal.hpp"
#include "common/foreach.hpp"
#ifdef WITH_ZOOKEEPER
#include "common/zookeeper.hpp"
#endif

#include "messaging/messages.hpp"

#include "detector.hpp"
#include "url_processor.hpp"

using namespace mesos;
using namespace mesos::internal;

using boost::lexical_cast;

using process::Process;
using process::UPID;

using std::pair;
using std::string;
using std::vector;


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
  ZooKeeperMasterDetector(const string& servers,
			  const string& znode,
			  const UPID& pid,
			  bool contend = false,
			  bool quiet = false);

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

  const string servers;
  const string znode;
  const UPID pid;
  bool contend;
  bool reconnect;

  ZooKeeper *zk;

  // Our sequence string if contending to be a master.
  string mySeq;

  string currentMasterSeq;
  UPID currentMasterPID;
};
#endif // WITH_ZOOKEEPER


MasterDetector::~MasterDetector() {}


MasterDetector* MasterDetector::create(const string &url,
                                       const UPID &pid,
                                       bool contend,
                                       bool quiet)
{
  if (url == "")
    if (contend) {
      return new BasicMasterDetector(pid);
    } else {
      fatal("cannot use specified url to detect master");
    }

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
      if (index == string::npos) {
	fatal("expecting chroot path for ZooKeeper");
      }

      const string &servers = urlPair.second.substr(0, index);

      const string &znode = urlPair.second.substr(index);
      if (znode == "/") {
	fatal("expecting chroot path for ZooKeeper ('/' is not supported)");
      }

      detector = new ZooKeeperMasterDetector(servers, znode, pid, contend, quiet);
#else
      fatal("Cannot detect masters with 'zoo://', "
            "ZooKeeper is not supported in this build");
#endif // WITH_ZOOKEEPER
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
	UPID master(urlPair.second);
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


BasicMasterDetector::BasicMasterDetector(const UPID& _master)
  : master(_master)
{
  // Send a master token.
  {
    MSG<GOT_MASTER_TOKEN> msg;
    msg.set_token("0");
    MesosProcess<class T>::post(master, msg);
  }

  // Elect the master.
  {
    MSG<NEW_MASTER_DETECTED> msg;
    msg.set_pid(master);
    MesosProcess<class T>::post(master, msg);
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
      MSG<GOT_MASTER_TOKEN> msg;
      msg.set_token("0");
      MesosProcess<class T>::post(master, msg);
    }

    // Elect the master.
    {
      MSG<NEW_MASTER_DETECTED> msg;
      msg.set_pid(master);
      MesosProcess<class T>::post(master, msg);
    }
  }

  // Tell the pid about the master.
  MSG<NEW_MASTER_DETECTED> msg;
  msg.set_pid(master);
  MesosProcess<class T>::post(pid, msg);
}


BasicMasterDetector::BasicMasterDetector(const UPID& _master,
					 const vector<UPID>& pids,
					 bool elect)
  : master(_master)
{
  if (elect) {
    // Send a master token.
    {
      MSG<GOT_MASTER_TOKEN> msg;
      msg.set_token("0");
      MesosProcess<class T>::post(master, msg);
    }

    // Elect the master.
    {
      MSG<NEW_MASTER_DETECTED> msg;
      msg.set_pid(master);
      MesosProcess<class T>::post(master, msg);
    }
  }

  // Tell each pid about the master.
  foreach (const UPID& pid, pids) {
    MSG<NEW_MASTER_DETECTED> msg;
    msg.set_pid(master);
    MesosProcess<class T>::post(pid, msg);
  }
}


BasicMasterDetector::~BasicMasterDetector() {}


#ifdef WITH_ZOOKEEPER
ZooKeeperMasterDetector::ZooKeeperMasterDetector(const string& servers,
						 const string& znode,
						 const UPID& pid,
						 bool contend,
						 bool quiet)
  : servers(servers), znode(znode), pid(pid),
    contend(contend), reconnect(false)
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
  }
}


void ZooKeeperMasterDetector::connected()
{
  LOG(INFO) << "Master detector connected to ZooKeeper ...";

  int ret;
  string result;

  static const string delimiter = "/";

  // Assume the znode that was created does not end with a "/".
  CHECK(znode.at(znode.length() - 1) != '/');

  // Create directory path znodes as necessary.
  size_t index = znode.find(delimiter, 0);

  while (index < string::npos) {
    // Get out the prefix to create.
    index = znode.find(delimiter, index + 1);
    string prefix = znode.substr(0, index);

    LOG(INFO) << "Trying to create znode '" << prefix << "' in ZooKeeper";

    // Create the node (even if it already exists).
    ret = zk->create(prefix, "", ZOO_OPEN_ACL_UNSAFE,
		     // ZOO_CREATOR_ALL_ACL, // needs authentication
		     0, &result);

    if (ret != ZOK && ret != ZNODEEXISTS) {
      fatal("failed to create ZooKeeper znode! (%s)", zk->error(ret));
    }
  }

  // Wierdness in ZooKeeper timing, let's check that everything is created.
  ret = zk->get(znode, false, &result, NULL);

  if (ret != ZOK) {
    fatal("ZooKeeper not responding correctly (%s). "
	  "Make sure ZooKeeper is running on: %s",
	  zk->error(ret), servers.c_str());
  }

  if (contend) {
    // We contend with the pid given in constructor.
    ret = zk->create(znode + "/", pid, ZOO_OPEN_ACL_UNSAFE,
		     // ZOO_CREATOR_ALL_ACL, // needs authentication
		     ZOO_SEQUENCE | ZOO_EPHEMERAL, &result);

    if (ret != ZOK) {
      fatal("ZooKeeper not responding correctly (%s). "
	    "Make sure ZooKeeper is running on: %s",
	    zk->error(ret), servers.c_str());
    }

    // Save the sequnce id but only grab the basename, e.g.,
    // "/path/to/znode/000000131" => "000000131".
    size_t index;
    if ((index = result.find_last_of('/')) != string::npos) {  
      mySeq = result.erase(0, index + 1);
    } else {
      mySeq = "";
    }

    LOG(INFO) << "Created ephemeral/sequence:" << mySeq;

    MSG<GOT_MASTER_TOKEN> msg;
    msg.set_token(mySeq);
    MesosProcess<class T>::post(pid, msg);
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

  int ret;
  string result;

  static const string delimiter = "/";

  if (contend) {
    // Contending for master, confirm our ephemeral sequence znode exists.
    ret = zk->get(znode + "/" + mySeq, false, &result, NULL);

    // We might no longer be the master! Commit suicide for now
    // (hoping another master is on standbye), but in the future
    // it would be nice if we could go back on standbye.
    if (ret == ZNONODE) {
      fatal("failed to reconnect to ZooKeeper quickly enough "
	    "(our ephemeral sequence znode is gone), commiting suicide!");
    }

    if (ret != ZOK) {
      fatal("ZooKeeper not responding correctly (%s). "
	    "Make sure ZooKeeper is running on: %s",
	    zk->error(ret), servers.c_str());
    }

    // We are still the master!
    LOG(INFO) << "Still acting as master";
  } else {
    // Reconnected, but maybe the master changed?
    detectMaster();
  }
}


void ZooKeeperMasterDetector::expired()
{
  LOG(WARNING) << "Master detector ZooKeeper session expired!";

  CHECK(zk != NULL);
  delete zk;

  zk = new ZooKeeper(servers, 10000, this);
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

  int ret = zk->getChildren(znode, true, &results);

  if (ret != ZOK) {
    LOG(ERROR) << "Master detector failed to get masters: "
	       << zk->error(ret);
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
    process::post(pid, NO_MASTER_DETECTED);
  } else if (masterSeq != currentMasterSeq) {
    // Okay, let's fetch the master pid from ZooKeeper.
    string result;
    ret = zk->get(znode + "/" + masterSeq, false, &result, NULL);

    if (ret != ZOK) {
      // This is possible because the master might have failed since
      // the invocation of ZooKeeper::getChildren above.
      LOG(ERROR) << "Master detector failed to fetch new master pid: "
		 << zk->error(ret);
      process::post(pid, NO_MASTER_DETECTED);
    } else {
      // Now let's parse what we fetched from ZooKeeper.
      LOG(INFO) << "Master detector got new master pid: " << result;

      UPID masterPid = result;

      if (masterPid == UPID()) {
	// TODO(benh): Maybe we should try again then!?!? Parsing
	// might have failed because of DNS, and whoever is using the
	// detector might sit "unconnected" indefinitely!
	LOG(ERROR) << "Failed to parse new master pid!";
	process::post(pid, NO_MASTER_DETECTED);
      } else {
	currentMasterSeq = masterSeq;
	currentMasterPID = masterPid;

	MSG<NEW_MASTER_DETECTED> msg;
	msg.set_pid(currentMasterPID);
	MesosProcess<class T>::post(pid, msg);
      }
    }
  }
}

#endif // WITH_ZOOKEEPER
