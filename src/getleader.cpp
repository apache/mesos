#include <iostream>
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <zookeeper.h>
#include "getleader.hpp"

#define debug(x) cout<<x

using namespace std;

// I don't really use this
void LeaderDetector::initWatchWrap(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx) {
  debug("Got called with type:"<<type);
}

// TODO ALI: Make this thread safe, will be called by ZooKeeper thread
void LeaderDetector::leaderWatchWrap(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx) {
  ((LeaderDetector *)watcherCtx)->leaderWatch(zh, type, state, path);
}


LeaderDetector::LeaderDetector(string server, bool contendLeader, string ld, LeaderListener *ll) : 
  leaderListener(ll),
  zh(NULL),mydata(ld),
  zooserver(server),currentLeaderId("")
{

  zh = zookeeper_init(zooserver.c_str(), initWatchWrap, 1000, 0, NULL, 0);
  debug("zhandle:"<<zh<<endl);


  int ret = zoo_create(zh, "/nxmaster",  NULL, -1, &ZOO_OPEN_ACL_UNSAFE, 0, buf, 100);
  debug("creation returned:"<<ret<<" ok is:"<<ZOK<<endl);

  if (contendLeader) {
    ret = zoo_create(zh, "/nxmaster/",  mydata.c_str(), mydata.length(), &ZOO_OPEN_ACL_UNSAFE, ZOO_SEQUENCE | ZOO_EPHEMERAL, buf, 100);
    debug("creation of ephemeral:"<<buf<<" returned:"<<ret<<" ok is:"<<ZOK<<endl);
  }

  detectLeader();
}

void LeaderDetector::leaderWatch(zhandle_t *zh, int type, int state, const char *path) {
  debug("Leader watch called with type:"<<type);
  if (type == ZOO_CREATED_EVENT) {
    debug("Event type: ZOO_CREATED_EVENT"<<endl<<flush);
  } else if (type == ZOO_DELETED_EVENT) {
    debug("Event type: ZOO_DELETED_EVENT"<<endl<<flush);
  } else if (type == ZOO_CHANGED_EVENT) {
    debug("Event type: ZOO_CHANGED_EVENT"<<endl<<flush);
  } else if (type == ZOO_CHILD_EVENT) {
    debug("Event type: ZOO_CHILD_EVENT"<<endl<<flush);
  } else if (type == ZOO_SESSION_EVENT) {
    debug("Event type: ZOO_SESSION_EVENT"<<endl<<flush);
  } else if (type == ZOO_NOTWATCHING_EVENT) {
    debug("Event type: ZOO_NOTWATCHING_EVENT"<<endl<<flush);
  }
  detectLeader();
}

bool LeaderDetector::detectLeader() {
  debug("calling:"<<zh<<" getchildren"<<endl<<flush);
  int zret = zoo_wget_children(zh, "/nxmaster", leaderWatchWrap, (void*)this, &sv);
  debug("zret of wget_children:"<<zret<<" "<<ZOK<<endl<<flush);

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
  debug("calling:"<<zh<<" get"<<endl<<flush);
  string path="/nxmaster/";
  path+=id;
  char buf[100];
  int buflen = sizeof(buf);
  int zret = zoo_get(zh, path.c_str(), 0, buf, &buflen, NULL);
  debug("zret of get:"<<zret<<" "<<ZOK<<" f:"<<buf[0]<<endl<<flush);
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

void LeaderDetector::newLeader(string leader, string leaderdata) {
  cout<<"## NEW LEADER ## "<<leader<<" data:"<<leaderdata<<endl<<flush;
  if (leaderListener!=NULL)
    leaderListener->newLeaderElected(leader,leaderdata);
}

LeaderDetector::~LeaderDetector() {
  zookeeper_close(zh);
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
