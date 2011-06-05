#ifndef __MASTER_DETECTOR_HPP__
#define __MASTER_DETECTOR_HPP__

#include <string>
#include <iostream>
#include <unistd.h>
#include <climits>
#include <cstdlib>

#include "zookeeper.hpp"

using namespace std;


/**
 * Implements functionality for a) detecting masters b) contending to be a master.
 */
class MasterDetector : public Watcher {
public:
  /** 
   * Connects to ZooKeeper and possibly contends for master.
   * 
   * @param server comma separated list of zookeeper server host:port
   *   pairs
   * @param znode ZooKeeper node (directory)
   * @param pid libprocess pid that we should send messages to (and to
   *   contend)
   * @param contend true if should contend to be master (not needed
   *   for slaves and frameworks)
   */
  MasterDetector(const string &server, const string &znode,
		 const PID &_pid, bool contend = false);

  ~MasterDetector();

  /** 
   * ZooKeeper watcher.
   */
  virtual void process(ZooKeeper *zk, int type, int state, const string &path);

  /** 
   * @return ZooKeeper unique sequence number of the current master.
   */
  string getCurrentMasterSeq() const;

  /** 
   * @return libprocess PID of the current master. 
   */
  PID getCurrentMasterPID() const;

  /**
   * @return Unique ZooKeeper sequence number (only if contending for master, otherwise "").
   */
  string getMySeq() const
  {
    return mySeq;
  }

  /**
   * Adjusts ZooKeepers level of debugging output.
   * @param quiet true makes ZK quiet, whereas false makes ZK output DEBUG messages
   */ 
  static void setQuiet(bool quiet)
  {
    zoo_set_debug_level(quiet ? ZOO_LOG_LEVEL_ERROR : ZOO_LOG_LEVEL_DEBUG);
  }

private: 
  void setMySeq(const string &s)
  {
    string seq = s;
    // Converts "/path/to/znode/000000131" to "000000131".
    int pos;
    if ((pos = seq.find_last_of('/')) != string::npos) {  
      mySeq = seq.erase(0, pos + 1);
    } else
      mySeq = "";
  }

  /*
  * TODO(alig): Comment this object.
  */
  void detectMaster();

  /*
  * TODO(alig): Comment this object.
  */
  PID lookupMasterPID(const string &seq) const;

  string servers;
  string znode;
  PID pid;
  bool contend;
  bool reconnect;

  ZooKeeper *zk;

  // Our sequence number if contending.
  string mySeq;

  string currentMasterSeq;
  PID currentMasterPID;
};

#endif /* __MASTER_DETECTOR_HPP__ */

