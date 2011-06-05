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

FTMessaging *FTMessaging::instance = NULL;

}}
