#include <iostream>
#include <unistd.h>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <zookeeper.h>
#include <glog/logging.h>
#include "messages.hpp"
#include "leader_detector.hpp"
#include "ft_messaging.hpp"

namespace nexus { namespace internal {

using namespace nexus;
using namespace nexus::internal;
using namespace std;
    
FTMessaging *FTMessaging::getInstance() {
  if (instance==NULL)
    instance = new FTMessaging();
  return instance;
}
    
FTMessaging *FTMessaging::getInstance(PID _master) {
  if (instance==NULL)
    instance = new FTMessaging(_master);
  return instance;
}
    
FTMessaging *FTMessaging::getInstance(string _master) {
  if (instance==NULL) {
    PID mPid;
    istringstream ss(_master);
    if (!(ss >> mPid)) {
      LOG(ERROR) << "Couldn't create PID out of string in constructor.";
      return NULL;
    }
    instance = new FTMessaging(mPid);
  }
  return instance;
}
    
FTMessaging::FTMessaging(PID _master) : 
  master(_master), msgId(0)
{ 
  srand(time(0));
  char s[50];
  sprintf(s, "%09i", (int)rand());
  uniqPrefix = s;
  DLOG(INFO) << "FT: Created unique FT TAG: " << s;
}

FTMessaging::FTMessaging() : 
  msgId(0)
{ 
  srand(time(0));
  char s[50];
  sprintf(s, "%09i", (int)rand());
  uniqPrefix = s;
  DLOG(INFO) << "FT: Created unique FT TAG: " << s;
}

string FTMessaging::getNextId() {
  return uniqPrefix + ":" + lexical_cast<string>(msgId++);
}                                                      
  
void FTMessaging::gotAck(string ftId) {
  DLOG(INFO) << "FT: Got ack, deleting outstanding msg " << ftId;
  outMsgs.erase(ftId);
}

void FTMessaging::sendOutstanding() {
  if (!master) {
    DLOG(INFO) << "FT: Not RE-resending due to NULL master PID";
    return;
  } 

  foreachpair( const string &ftId, struct FTStoredMsg &msg, outMsgs) {
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
bool FTMessaging::acceptMessage(string from, string ftId) {
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

bool FTMessaging::acceptMessageAck(string from, string ftId) {
  DLOG(INFO) << "FT: Received message with id: " << ftId << " sending FT_RELAY_ACK";

  bool res = acceptMessage(from, ftId);

  if (!res) {
    LOG(WARNING) << "FT: asked caller to ignore duplicate message " << ftId;
    return res;
  }  

  string msgStr = Tuple<EmptyClass>::tupleToString( Tuple<EmptyClass>::pack<FT_RELAY_ACK>(ftId, from) );
  Process::post(master, FT_RELAY_ACK, msgStr.data(), msgStr.size()); 

  return res;
}

void FTMessaging::setMasterPid(const PID &mPid) {
  master = mPid;
}

FTMessaging *FTMessaging::instance = NULL;

}}
