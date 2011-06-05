#include <iostream>
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <zookeeper.h>
#include <glog/logging.h>
#include "leader_detector.hpp"

using namespace std;

// I don't really use this
void LeaderDetector::initWatchWrap(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx) {

}

// TODO ALI: Make this thread safe, will be called by ZooKeeper thread
void LeaderDetector::leaderWatchWrap(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx) {
  ((LeaderDetector *)watcherCtx)->leaderWatch(zh, type, state, path);
}


LeaderDetector::LeaderDetector(string server, bool contendLeader, string pid, LeaderListener *ll) : 
  leaderListener(ll),
  zh(NULL),myPID(pid),
  zooserver(server),currentLeaderSeq(""), mySeq("")
{

  zh = zookeeper_init(zooserver.c_str(), initWatchWrap, 1000, 0, NULL, 0);
  LOG(INFO) << "Initialized ZooKeeper";
  LOG_IF(ERROR, zh==NULL)<<"zookeeper_init returned error:";


  char buf[100];

  int ret = zoo_create(zh, "/nxmaster",  NULL, -1, &ZOO_OPEN_ACL_UNSAFE, 0, buf, 100);

  if (contendLeader) {
    ret = zoo_create(zh, "/nxmaster/",  myPID.c_str(), myPID.length(), &ZOO_OPEN_ACL_UNSAFE, ZOO_SEQUENCE | ZOO_EPHEMERAL, buf, 100);
    LOG_IF(ERROR, ret!=ZOK)<<"zoo_create() ephemeral/sequence returned error:"<<ret;
    if (ret==ZOK) {
      setMySeq(string(buf));
      LOG(INFO)<<"Created ephemeral/sequence:"<<mySeq;
    }
  }

  detectLeader();
}

void LeaderDetector::leaderWatch(zhandle_t *zh, int type, int state, const char *path) {
  if (type == ZOO_CREATED_EVENT) {
    LOG(INFO)<<"Leader Watch Event type: ZOO_CREATED_EVENT";
  } else if (type == ZOO_DELETED_EVENT) {
    LOG(INFO)<<"Leader Watch Event type: ZOO_DELETED_EVENT";
  } else if (type == ZOO_CHANGED_EVENT) {
    LOG(INFO)<<"Leader Watch Event type: ZOO_CHANGED_EVENT";
  } else if (type == ZOO_CHILD_EVENT) {
    LOG(INFO)<<"Leader Watch Event type: ZOO_CHILD_EVENT";
  } else if (type == ZOO_SESSION_EVENT) {
    LOG(INFO)<<"Leader Watch Event type: ZOO_SESSION_EVENT";
  } else if (type == ZOO_NOTWATCHING_EVENT) {
    LOG(INFO)<<"Leader Watch Event type: ZOO_NOTWATCHING_EVENT";
  }
  detectLeader();
}

bool LeaderDetector::detectLeader() {
  String_vector sv;
  int ret = zoo_wget_children(zh, "/nxmaster", leaderWatchWrap, (void*)this, &sv);
  LOG_IF(ERROR, ret!=ZOK)<<"zoo_wget_children (get leaders) returned error:"<<ret;
  LOG_IF(INFO, ret==ZOK)<<"zoo_wget_children returned "<<sv.count<<" registered leaders";
  
  string leader;
  long min = LONG_MAX;
  for (int x=0; x<sv.count; x++) {
    int i = atol(sv.data[x]);
    if (i<min) {
      min=i;
      leader = sv.data[x];
    }
  }

  if (leader!=currentLeaderSeq) {
    currentLeaderSeq=leader;

    string data = fetchLeaderPID(leader); 
    
    newLeader(leader, data); // both params could be ""
      
    return 1; 
  }
  return 0;
}

string LeaderDetector::fetchLeaderPID(string id) {
  if (id=="") {
    currentLeaderPID = "";
    return currentLeaderPID;
  }
  string path="/nxmaster/";
  path+=id;
  char buf[100];
  int buflen = sizeof(buf);
  int ret = zoo_get(zh, path.c_str(), 0, buf, &buflen, NULL);
  LOG_IF(ERROR, ret!=ZOK)<<"zoo_get returned error:"<<ret;
  LOG_IF(INFO, ret==ZOK)<<"zoo_get leader data fetch returned "<<buf[0];

  string tmp(buf,buflen);
  currentLeaderPID=tmp;
  return currentLeaderPID;
}

string LeaderDetector::getCurrentLeaderSeq() {
  return currentLeaderSeq;
}

string LeaderDetector::getCurrentLeaderPID() {
  return currentLeaderPID;
}

void LeaderDetector::newLeader(string leader, string leaderPID) {
  LOG(INFO)<<"New leader ephemeral_id:"<<leader<<" data:"<<leaderPID;
  if (leaderListener!=NULL)
    leaderListener->newLeaderElected(leader,leaderPID);
}

LeaderDetector::~LeaderDetector() {
  int ret = zookeeper_close(zh);
  LOG_IF(ERROR, ret!=ZOK)<<"zookeeper_close returned error:"<<ret;
  LOG_IF(INFO, ret==ZOK)<<"zookeeper_close OK";
}

/*
int main(int argc, char **argv) {
  string name="testing";
  if (argc==2)
    name=argv[1];
  LeaderDetector ld(1, name);
  string s = ld.getCurrentLeaderSeq();
  debug("Leader is "<<s<<endl<<flush);
  
  sleep(50);
  return 0;
}
*/
