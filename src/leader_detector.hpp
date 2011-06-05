#ifndef _LEADER_DETECTOR_HPP_
#define _LEADER_DETECTOR_HPP_

#include <string>
#include <iostream>
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <zookeeper.h>

using namespace std;

/**
 * Callback interface for LeaderDetector.
 */
class LeaderListener {
public:
  /** Callback method
   *
   * @param zkId ZooKeeper sequence number of the new leader
   * @param pidStr libprocess PID of the new leader
   */ 
  virtual void newLeaderElected(string zkId, string pidStr) = 0;
};

/**
 * Implements functionality for a) detecting leaders b) contending to be a leader.
 */
class LeaderDetector {
public:
  /** 
   * Contact ZooKeeper, possibly contend for leader, and register callback.
   * 
   * @param server comma separated list of zookeeper servers
   * @param contendLeader true if object should try to become a leader (not needed for slaves and frameworks)
   * @param pid string containing libprocess id of this node (needed if it becomes a leader)
   * @param ll callback object which will be invoked each time a new leader is elected
   */
  LeaderDetector(string server, bool contendLeader=0, string pid="", LeaderListener * ll=NULL);

  /** 
   * @return ZooKeeper unique sequence number of the current leader.
   */
  string getCurrentLeaderSeq();

  /** 
   * @return libprocess PID of the current leader. 
   */
  string getCurrentLeaderPID();

  /**
   * Registers a listener that gets callbacks when a new leader is elected.
   *
   * @param ll an object implementing LeaderListener
   */
  void setListener(LeaderListener *l) {
    leaderListener = l;
  }

  /**
   * @return Unique ZooKeeper sequence number (only if contending for leader, otherwise "").
   */
  string getMySeq() const {
    return mySeq;
  }

  ~LeaderDetector();

private: 
  static void initWatchWrap(zhandle_t * zh, int type, int state, const char *path, void *watcherCtx);

  static void leaderWatchWrap(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx);

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
  string myPID;
  string zooserver;
  string currentLeaderSeq;
  string currentLeaderPID;
  string mySeq;

};

#endif /* _LEADER_DETECTOR_HPP_ */
