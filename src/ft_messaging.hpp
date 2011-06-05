#ifndef _FT_MESSAGING_HPP_
#define _FT_MESSAGING_HPP_

#define FT_MAX_RESENDS 30 // 5 minutes

#include <boost/lexical_cast.hpp>
#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>
#include <process.hpp>
#include <iostream>
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <zookeeper.h>
#include <glog/logging.h>
#include <ctime>
#include <cstdlib>
#include "leader_detector.hpp"


#include <dirent.h>
#include <libgen.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <strings.h>

#include <iostream>
#include <list>
#include <sstream>
#include <vector>

#include <arpa/inet.h>

#include <boost/lexical_cast.hpp>
#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>

#include <glog/logging.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <process.hpp>

#include "messages.hpp"
#include "tuple.hpp"
#include "foreach.hpp"


namespace nexus { namespace internal {

using namespace nexus;
using namespace nexus::internal;
using namespace std;
using boost::lexical_cast;
using boost::unordered_map;
using boost::unordered_set;

class EmptyClass {
};

/**
 * Used in FTMessaging to store unacked messages, their count, ftId, libprocess id.
 * @see FTMessaging
 */
struct FTStoredMsg {

  FTStoredMsg(const string &_ftId, const string &_data, const MSGID &_id) : 
    ftId(_ftId), data(_data), id(_id), count(0) {}

  FTStoredMsg() : ftId(""), data(""), id(), count(0) {}

  string ftId;
  string data;
  MSGID id;
  long count;
};


/**
 * Singleton class that provides functionality for reliably sending messages, 
 * resending them on timeout, acking received messages, and dropping duplicates.
 */
class FTMessaging {
public:
  /**
   * @return A singleton instance of this class, the master string needs to be set.
   * @see setMasterPid()
   */
  static FTMessaging *getInstance();

  /**
   * @return A singleton instance of this class.
   * @param master libprocess PID to current master
   */
  static FTMessaging *getInstance(PID master);

  /**
   * @return A singleton instance of this class.
   * @param masterStr string representing libprocess PID to current master (no nexus:// prefix)
   */
  static FTMessaging *getInstance(string masterStr);

  /**
   * Reliably sends a message with a given fault tolerant id 
   * The message will also be stored in a pending set.
   * @see getNextId().
   * @param ftId string representing the unique FT id of the message
   * @param msgTuple libprocess tuple<ID> 
   */
  template<MSGID ID> void reliableSend(const string &ftId, const tuple<ID> &msgTuple)
  {
    DLOG(INFO) << "FT: sending " << ftId;
    string msgStr = Tuple<EmptyClass>::tupleToString(msgTuple);
    FTStoredMsg sm(ftId, msgStr, ID);
    outMsgs[ftId] = sm;
    if (!master) {
      DLOG(INFO) << "FT: Not RE-resending due to NULL master PID";
      return;
    }  else
      Process::post(master, ID, msgStr.data(), msgStr.size());
  }

  /**
   * Removes any pending message with a given id. This is to be called upon the receipt of a message.
   * @param ftId string representing the unique FT id of the message.
   */
  void gotAck(string ftId);

  /**
   * Attempts to send all pending messages to the current master. Pending messages are messages that have not been acked yet.
   */
  void sendOutstanding();

  /**
   * Checks if a message with FT ID from a node has already been received previously. 
   * @param from libprocess PID string representing the original sender of the message
   * @param ftId the FT ID of the message
   * @return true if message has not been received before and it is the next message expected to be received, false otherwise.
   */
  bool acceptMessage(string from, string ftId);

  /**
   * Same as acceptMessage, but also sends an ACK back to the original sender if it returns true.
   * @param from libprocess PID string representing the original sender of the message
   * @param ftId the FT ID of the message
   * @return true if message has not been received before and it is the next message expected to be received, false otherwise.
   */
  bool acceptMessageAck(string from, string ftId);

  /**
   * @return a new unique FT ID for a message to be sent
   */ 
  string getNextId();

  /**
   * Sets the PID to the master (to be called when a new master comes up).
   * Important invariant: needs to be called every time the master changes in slave/master/sched.
   * @param mPid PID to the current master
   */
  void setMasterPid(const PID &mPid);

private:

  PID master;

  unordered_map<string, FTStoredMsg> outMsgs;

  unordered_map<string, string> inMsgs;

  string uniqPrefix;

  long msgId;

  static FTMessaging *instance;

  FTMessaging();
  FTMessaging(PID _master);

  FTMessaging(FTMessaging const &copy) {}
  FTMessaging &operator= (FTMessaging const &copy) {}
};

}}

#endif /* _FT_MESSAGING_HPP_ */
