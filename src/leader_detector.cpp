#include <iostream>
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <zookeeper.h>
#include <boost/lexical_cast.hpp>
#include <glog/logging.h>
#include <stdexcept>
#include "leader_detector.hpp"

using namespace std;

using boost::lexical_cast;

// I don't really use this
void LeaderDetector::initWatchWrap(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx) {

}

// TODO ALI: Make this thread safe, will be called by ZooKeeper thread
void LeaderDetector::leaderWatchWrap(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx) {
  ((LeaderDetector *) watcherCtx)->leaderWatch(zh, type, state, path);
}


LeaderDetector::LeaderDetector(const string &server, const bool contendLeader, const string &pid, 
                               LeaderListener *ll) : 
  leaderListener(ll),
  zh(NULL),myPID(pid),
  zooserver(server),currentLeaderSeq(""), mySeq("")
{

  zh = zookeeper_init(zooserver.c_str(), initWatchWrap, 1000, 0, NULL, 0);

  if (zh == NULL)
    throw runtime_error("ZooKeeper not responding correctly. Make sure ZooKeeper is running on: "
                        + zooserver);

  char buf[256];
  int buflen;
  int zret;

  buflen = sizeof(buf);
  zret = zoo_create(zh, "/nxmaster",  NULL, -1, &ZOO_OPEN_ACL_UNSAFE, 0, buf, buflen); // might already exist

  zret = zoo_get(zh, "/nxmaster", 0, buf, &buflen, NULL); // just to test zookeeper

  if (zret != ZOK)
    throw runtime_error("ZooKeeper not responding correctly (ret:" + lexical_cast<string>(zret) + 
                        "). Make sure ZooKeeper is running on: " + zooserver);

  if (contendLeader) {
    buflen = sizeof(buf);
    zret = zoo_create(zh, "/nxmaster/",  myPID.c_str(), myPID.length(), 
                     &ZOO_OPEN_ACL_UNSAFE, ZOO_SEQUENCE | ZOO_EPHEMERAL, buf, buflen);
    if (zret != ZOK)
      throw runtime_error("ZooKeeper not responding correctly (ret:" + lexical_cast<string>(zret) + 
                          "). Make sure ZooKeeper is running on: " + zooserver);

    if (zret == ZOK) {
      setMySeq(string(buf));
      LOG(INFO) << "Created ephemeral/sequence:" << mySeq;
    }
  }

  detectLeader();
}

void LeaderDetector::leaderWatch(zhandle_t *zh, int type, int state, const char *path) {
  if (type == ZOO_CREATED_EVENT) {
    DLOG(INFO) << "Leader Watch Event type: ZOO_CREATED_EVENT";
  } else if (type == ZOO_DELETED_EVENT) {
    DLOG(INFO) << "Leader Watch Event type: ZOO_DELETED_EVENT";
  } else if (type == ZOO_CHANGED_EVENT) {
    DLOG(INFO) << "Leader Watch Event type: ZOO_CHANGED_EVENT";
  } else if (type == ZOO_CHILD_EVENT) {
    DLOG(INFO) << "Leader Watch Event type: ZOO_CHILD_EVENT";
  } else if (type == ZOO_SESSION_EVENT) {
    DLOG(INFO) << "Leader Watch Event type: ZOO_SESSION_EVENT";
  } else if (type == ZOO_NOTWATCHING_EVENT) {
    DLOG(INFO) << "Leader Watch Event type: ZOO_NOTWATCHING_EVENT";
  }
  detectLeader();
}

bool LeaderDetector::detectLeader() {
  String_vector sv;
  int zret = zoo_wget_children(zh, "/nxmaster", leaderWatchWrap, (void*)this, &sv);
  LOG_IF(ERROR, zret != ZOK) << "zoo_wget_children (get leaders) returned error:" << zret;
  LOG_IF(INFO, zret == ZOK) << "zoo_wget_children returned " << sv.count << " registered leaders";
  
  string leader;
  long min = LONG_MAX;
  for (int x = 0; x<sv.count; x++) {
    int i = atol(sv.data[x]);
    if (i < min) {
      min = i;
      leader = sv.data[x];
    }
  }

  if (leader != currentLeaderSeq) {
    currentLeaderSeq = leader;

    string data = fetchLeaderPID(leader); 
    
    newLeader(leader, data); // both params could be ""
      
    return 1; 
  }
  return 0;
}

string LeaderDetector::fetchLeaderPID(const string &id) {
  if (id == "") {
    currentLeaderPID = "";
    return currentLeaderPID;
  }

  string path = "/nxmaster/";
  path += id;
  char buf[256];
  int buflen;
  buflen = sizeof(buf);
  int zret = zoo_get(zh, path.c_str(), 0, buf, &buflen, NULL);
  LOG_IF(ERROR, zret != ZOK) << "zoo_get returned error:" << zret;
  LOG_IF(INFO, zret == ZOK) << "zoo_get leader data fetch returned " << buf[0];

  string tmp(buf,buflen);
  currentLeaderPID = tmp;
  return currentLeaderPID;
}

string LeaderDetector::getCurrentLeaderSeq() const {
  return currentLeaderSeq;
}

string LeaderDetector::getCurrentLeaderPID() const {
  return currentLeaderPID;
}

void LeaderDetector::newLeader(const string &leader, const string &leaderPID) {
  LOG(INFO) << "New leader ephemeral_id:" << leader << " data:" << leaderPID;
  if (leaderListener != NULL)
    leaderListener->newLeaderElected(leader,leaderPID);
}

LeaderDetector::~LeaderDetector() {
  int zret = zookeeper_close(zh);
  LOG_IF(ERROR, zret != ZOK) << "zookeeper_close returned error:" << zret;
  LOG_IF(INFO, zret == ZOK) << "zookeeper_close OK";
}
