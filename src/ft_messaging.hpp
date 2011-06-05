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

struct StoredMsg {
  StoredMsg(const string &_ftId, const string &_data, const MSGID &_id) : 
    ftId(_ftId), data(_data), id(_id), count(0) {}
  StoredMsg() : ftId(""), data(""), id(), count(0) {}
  string ftId;
  string data;
  MSGID id;
  long count;
};


class FTMessaging {
public:
  static FTMessaging *getInstance();
  static FTMessaging *getInstance(PID master);
  static FTMessaging *getInstance(string masterStr);

  template<MSGID ID> void reliableSend(const string &ftId, const tuple<ID> &msgTuple)
  {
    DLOG(INFO) << "FT: sending " << ftId;
    string msgStr = Tuple<EmptyClass>::tupleToString(msgTuple);
    StoredMsg sm(ftId, msgStr, ID);
    outMsgs[ftId] = sm;
    if (!master) {
      DLOG(INFO) << "FT: Not RE-resending due to NULL master PID";
      return;
    }  else
    Process::post(master, ID, msgStr.data(), msgStr.size());
  }
  
  void gotAck(string ftId) {
    DLOG(INFO) << "FT: Got ack, deleting outstanding msg " << ftId;
    outMsgs.erase(ftId);
  }

  void sendOutstanding() {
    if (!master) {
      DLOG(INFO) << "FT: Not RE-resending due to NULL master PID";
      return;
    } 

    foreachpair( const string &ftId, struct StoredMsg &msg, outMsgs) {
      if (msg.count < FT_MAX_RESENDS) {
        DLOG(INFO) << "FT: RE-sending " << msg.ftId << " attempt:" << msg.count;
        Process::post(master, msg.id, msg.data.data(), msg.data.size());
        msg.count++;
      } else {
        DLOG(INFO) << "FT: Not RE-sending " << msg.ftId << " reached limit " << FT_MAX_RESENDS;
        outMsgs.erase(ftId);
      }
    }
  }

  // Careful: not idempotent function.
  bool acceptMessage(string from, string ftId) {
    if (inMsgs.find(from)==inMsgs.end()) {
      DLOG(INFO) << "FT: new msgs seq: " << ftId;
      inMsgs[from] = ftId;
      return true;
    } else {
      string oldSeq = inMsgs[from]; 
      string oldRnd = oldSeq;
      int pos;
      if ((pos=oldSeq.find_last_of(':'))!=string::npos ) {  
        oldSeq.erase(0,pos+1);
        oldRnd.erase(pos,255);
        long seqNr = lexical_cast<long>(oldSeq);
        string nextFtId = oldRnd+":"+lexical_cast<string>(seqNr+1);
        if (nextFtId==ftId) {
          DLOG(INFO) << "FT: match - got ftId:" << ftId << " expecting " << nextFtId;
          inMsgs[from] = nextFtId;
          return true;
        } else {
          DLOG(INFO) << "FT: mismatch - got ftId:" << ftId << " expecting " << nextFtId;
          return false;
        }
      } else {
        DLOG(INFO) << "FT: Error parsing ftId in acceptMessage for ftId:" << ftId;
        return false;
      }
    }
  }

  string getNextId();

  void setMasterPid(const PID &mPid) {
    master = mPid;
  }

private:

  PID master;

  unordered_map<string, StoredMsg> outMsgs;

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
