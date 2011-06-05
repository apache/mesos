#ifndef GETLEADER_HPP_
#define GETLEADER_HPP_


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
  virtual void newLeaderElected(string, string) = 0;
};

class LeaderDetector {
public:
  LeaderDetector(string, bool =0, string ="", LeaderListener * =NULL);

  void leaderWatch(zhandle_t *, int, int, const char *);

  pair<string,string> getCurrentLeader();

  string getCurrentLeaderId();

  string getCurrentLeaderData();

  void setListener(LeaderListener *l) {
    leaderListener = l;
  }

  LeaderListener *getListener() const {
    return leaderListener;
  }

  ~LeaderDetector();

  static void initWatchWrap(zhandle_t *, int, int, const char *, void *);

  static void leaderWatchWrap(zhandle_t *, int, int , const char *, void *);

private: 
  bool detectLeader();
  string fetchLeaderData(string);
  void newLeader(string,string);

  LeaderListener *leaderListener;
  zhandle_t *zh;
  string mydata;
  string zooserver;
  string currentLeaderId;
  string currentLeaderData;

  String_vector sv;
  char buf[100];

};

#endif /* GETLEADER_HPP_ */
