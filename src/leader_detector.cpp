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


LeaderDetector::LeaderDetector(string server, bool contendLeader, string ld, LeaderListener *ll) : 
  leaderListener(ll),
  zh(NULL),mydata(ld),
  zooserver(server),currentLeaderId(""), mySequence("")
{

  zh = zookeeper_init(zooserver.c_str(), initWatchWrap, 1000, 0, NULL, 0);
  LOG(INFO) << "Initialized ZooKeeper";
  LOG_IF(ERROR, zh==NULL)<<"zookeeper_init returned error:";


  char buf[100];

  int ret = zoo_create(zh, "/nxmaster",  NULL, -1, &ZOO_OPEN_ACL_UNSAFE, 0, buf, 100);

  if (contendLeader) {
    ret = zoo_create(zh, "/nxmaster/",  mydata.c_str(), mydata.length(), &ZOO_OPEN_ACL_UNSAFE, ZOO_SEQUENCE | ZOO_EPHEMERAL, buf, 100);
    LOG_IF(ERROR, ret!=ZOK)<<"zoo_create() ephemeral/sequence returned error:"<<ret;
    if (ret==ZOK) {
      setSequence(string(buf));
      LOG(INFO)<<"Created ephemeral/sequence:"<<mySequence;
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

  if (leader!=currentLeaderId) {
    currentLeaderId=leader;

    string data = fetchLeaderData(leader); 
    
    newLeader(leader, data); // both params could be ""
      
    return 1; 
  }
  return 0;
}

string LeaderDetector::fetchLeaderData(string id) {
  if (id=="") {
    currentLeaderData = "";
    return currentLeaderData;
  }
  string path="/nxmaster/";
  path+=id;
  char buf[100];
  int buflen = sizeof(buf);
  int ret = zoo_get(zh, path.c_str(), 0, buf, &buflen, NULL);
  LOG_IF(ERROR, ret!=ZOK)<<"zoo_get returned error:"<<ret;
  LOG_IF(INFO, ret==ZOK)<<"zoo_get leader data fetch returned "<<buf[0];

  string tmp(buf,buflen);
  currentLeaderData=tmp;
  return currentLeaderData;
}

pair<string,string> LeaderDetector::getCurrentLeader() {
  return pair<string,string>(getCurrentLeaderId(),getCurrentLeaderData());
}

string LeaderDetector::getCurrentLeaderId() {
  return currentLeaderId;
}

string LeaderDetector::getCurrentLeaderData() {
  return currentLeaderData;
}

void LeaderDetector::newLeader(string leader, string leaderData) {
  LOG(INFO)<<"New leader ephemeral_id:"<<leader<<" data:"<<leaderData;
  if (leaderListener!=NULL)
    leaderListener->newLeaderElected(leader,leaderData);
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
  string s = ld.getCurrentLeaderId();
  debug("Leader is "<<s<<endl<<flush);
  
  sleep(50);
  return 0;
}
*/
