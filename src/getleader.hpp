#ifndef GETLEADER_HPP_
#define GETLEADER_HPP_

#include <string>
#include <iostream>
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <zookeeper.h>

using namespace std;

class LeaderListener {
public:
  // first parameter is the ephemeral id of the leader, second parameter is leader data (could be a libprocess PID)
  // both parameters will be "" if there is no leader
  virtual void newLeaderElected(string zkId, string pidStr) = 0;
};

class LeaderDetector {
public:
  LeaderDetector(string server, bool contendLeader=0, string ld="", LeaderListener * ll=NULL);

  void leaderWatch(zhandle_t *zh, int type, int state, const char *path);

  pair<string,string> getCurrentLeader();

  string getCurrentLeaderId();

  string getCurrentLeaderData();

  void setListener(LeaderListener *l) {
    leaderListener = l;
  }

  LeaderListener *getListener() const {
    return leaderListener;
  }

  void setSequence(string seq) {  // converts "/nxmaster/000000131" to "000000131"
    int pos;
    if ((pos=seq.find_last_of('/'))!=string::npos ) {  
      mySequence = seq.erase(0,pos+1);
    } else
      mySequence = "";
  }

  string getSequence() const {
    return mySequence;
  }

  ~LeaderDetector();

  static void initWatchWrap(zhandle_t * zh, int type, int state, const char *path, void *watcherCtx);

  static void leaderWatchWrap(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx);

private: 
  bool detectLeader();
  string fetchLeaderData(string id);
  void newLeader(string leader,string leaderData);

  LeaderListener *leaderListener;
  zhandle_t *zh;
  string mydata;
  string zooserver;
  string currentLeaderId;
  string currentLeaderData;
  string mySequence;

  String_vector sv;

};

#endif /* GETLEADER_HPP_ */
