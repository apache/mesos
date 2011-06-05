#ifndef _LEADER_DETECTOR_HPP_
#define _LEADER_DETECTOR_HPP_

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

  pair<string,string> getCurrentLeader();

  string getCurrentLeaderSeq();

  string getCurrentLeaderPID();

  void setListener(LeaderListener *l) {
    leaderListener = l;
  }

  LeaderListener *getListener() const {
    return leaderListener;
  }

  string getMySeq() const {
    return mySeq;
  }

  ~LeaderDetector();

  static void initWatchWrap(zhandle_t * zh, int type, int state, const char *path, void *watcherCtx);

  static void leaderWatchWrap(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx);

private: 
  void setMySeq(string seq) {  // converts "/nxmaster/000000131" to "000000131"
    int pos;
    if ((pos=seq.find_last_of('/'))!=string::npos ) {  
      mySeq = seq.erase(0,pos+1);
    } else
      mySeq = "";
  }

  void leaderWatch(zhandle_t *zh, int type, int state, const char *path);
  bool detectLeader();
  string fetchLeaderPID(string id);
  void newLeader(string leader,string leaderPID);

  LeaderListener *leaderListener;
  zhandle_t *zh;
  string mydata;
  string zooserver;
  string currentLeaderSeq;
  string currentLeaderPID;
  string mySeq;

  String_vector sv;

};

#endif /* _LEADER_DETECTOR_HPP_ */
