#include <functional>
#include <list>
#include <queue>
#include <vector>

#include <boost/lexical_cast.hpp>

#include <process/dispatch.hpp>

#include "fatal.hpp"
#include "utils.hpp"
#include "zookeeper.hpp"
#include "zookeeper_candidate.hpp"

using std::list;
using std::make_pair;
using std::pair;
using std::queue;
using std::string;
using std::vector;


namespace mesos { namespace internal { namespace zookeeper {

class ElectionProcess : public Process<ElectionProcess>
{
public:
  ElectionProcess(Election* _election,
                  const string& _servers,
                  const string& _znode);

  virtual ~ElectionProcess();

  // Election method implementations.
  void submit(const PID<Candidate>& candidate, const string& data);
  void resign(const PID<Candidate>& candidate);

  // ZooKeeper events. TODO(*): Use a ZooKeeper listener?
  void connected();
  void reconnecting();
  void reconnected();
  void expired();
  void updated(const string& path);

private:
  // Runs an election.
  void elect();

  // Helpers for registering candidates in ZooKeeper.
  void registerCandidate(const PID<Candidate>& candidate, const string& data);
  void registerCandidates();

  // Pointer to election.
  Election* election;

  // ZooKeeper bits and pieces.
  bool ready;
  string servers;
  string znode;
  ZooKeeper* zk;
  Watcher* watcher;

  // Information about each of the candidates.
  struct CandidateInfo
  {
    PID<Candidate> pid;
    string data;
    uint64_t id;
    bool elected;
  };

  // Submitted but not registered candiates.
  queue<pair<PID<Candidate>, string> > queue;

  // Registered candidate information.
  list<CandidateInfo*> infos;
};


// A watcher that just forwards ZooKeeper events to election.
class ElectionProcessWatcher : public Watcher
{
public:
  ElectionProcessWatcher(const PID<ElectionProcess>& _pid)
    : pid(_pid), reconnect(false) {}

  virtual ~ElectionProcessWatcher() {}

  virtual void process(ZooKeeper* zk, int type, int state, const string& path)
  {
    if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_SESSION_EVENT)) {
      // Check if this is a reconnect.
      if (!reconnect) {
        // Initial connect.
        dispatch(pid, &ElectionProcess::connected);
      } else {
        // Reconnected.
        dispatch(pid, &ElectionProcess::reconnected);
      }
    } else if ((state == ZOO_CONNECTING_STATE) &&
               (type == ZOO_SESSION_EVENT)) {
      // The client library automatically reconnects, taking into
      // account failed servers in the connection string,
      // appropriately handling the "herd effect", etc.
      reconnect = true;
      dispatch(pid, &ElectionProcess::reconnecting);
    } else if ((state == ZOO_EXPIRED_SESSION_STATE) &&
               (type == ZOO_SESSION_EVENT)) {
      dispatch(pid, &ElectionProcess::expired);

      // If this watcher is reused, the next connect won't be a reconnect.
      reconnect = false;
    } else if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_CHILD_EVENT)) {
      dispatch(pid, &ElectionProcess::updated, path);
    } else if ((state == ZOO_CONNECTED_STATE) && (type == ZOO_CHANGED_EVENT)) {
      dispatch(pid, &ElectionProcess::updated, path);
    } else {
      LOG(FATAL) << "Unimplemented ZooKeeper event: (state is "
                 << state << " and type is " << type << ")";
    }
  }

private:
  const PID<ElectionProcess> pid;
  bool reconnect;
};


ElectionProcess::ElectionProcess(Election* _election,
                                 const string& _servers,
                                 const string& _znode)
  : election(_election), servers(_servers), znode(_znode)
{
  // TODO(benh): Real testing requires injecting a ZooKeeper instance.
  watcher = new ElectionProcessWatcher(self());
  zk = new ZooKeeper(servers, 10000, watcher);
}


ElectionProcess::~ElectionProcess()
{
  delete zk;
  delete watcher;

  foreach (CandidateInfo* info, infos) {
    delete info;
  }

  infos.clear();
}


void ElectionProcess::submit(
    const PID<Candidate>& candidate,
    const string& data)
{
  if (!ready) {
    queue.push(make_pair(candidate, data));
  } else {
    registerCandidate(candidate, data);
  }
}


void ElectionProcess::resign(const PID<Candidate>& candidate)
{
  // TODO(jsirois): Implement me!
  LOG(FATAL) << "unimplemented";
}


void ElectionProcess::connected()
{
  LOG(INFO) << "Election connected to ZooKeeper";

  int ret;
  string result;

  // Assume the znode that was created does not end with a "/".
  CHECK(znode.size() == 0 || znode.at(znode.size() - 1) != '/');

  // Create directory path znodes as necessary.
  size_t index = znode.find("/", 0);

  while (index < string::npos) {
    // Get out the prefix to create.
    index = znode.find("/", index + 1);
    string prefix = znode.substr(0, index);

    LOG(INFO) << "Election trying to create znode '"
              << prefix << "' in ZooKeeper";

    // Create the node (even if it already exists).
    ret = zk->create(prefix, "", ZOO_OPEN_ACL_UNSAFE,
		     // ZOO_CREATOR_ALL_ACL, // needs authentication
		     0, &result);

    if (ret != ZOK && ret != ZNODEEXISTS) {
      LOG(FATAL) << "Failed to create '" << prefix
                 << "' in ZooKeeper: " << zk->error(ret);
    }
  }

  // Signal that ZooKeeper is ready for use.
  ready = true;

  // Check if there are any submitted candidates that need to get
  // created in ZooKeeper.
  registerCandidates();

  // Run an initial election!
  elect();
}


void ElectionProcess::reconnecting()
{
  // TODO(jsirois): Implement me!
  LOG(FATAL) << "unimplemented";
}


void ElectionProcess::reconnected()
{
  // TODO(jsirois): Implement me!
  LOG(FATAL) << "unimplemented";
}


void ElectionProcess::expired()
{
  // TODO(jsirois): Implement me!
  LOG(FATAL) << "unimplemented";
}


void ElectionProcess::updated(const string& path)
{
  CHECK(znode == path);
  elect();
}


void ElectionProcess::elect()
{
  // Determine which replica is the master/coordinator.
  vector<string> results;

  int ret = zk->getChildren(znode, true, &results);

  if (ret != ZOK) {
    LOG(FATAL) << "Failed to get children of '" << znode
               << "' in ZooKeeper: " << zk->error(ret);
  }

  // "Elect" the minimum ephemeral znode.
  uint64_t id = LONG_MAX;
  foreach (const string& result, results) {
    try {
      id = std::min(id, boost::lexical_cast<uint64_t>(result));
    } catch (boost::bad_lexical_cast&) {
      LOG(FATAL) << "Failed to convert '" << result << "' into an integer";
    }
  }

  foreach (CandidateInfo* info, infos) {
    if (info->id == id) {
      info->elected = true;
      dispatch(info->pid, &Candidate::onElected, election);
    } else if (info->elected) {
      dispatch(info->pid, &Candidate::onDefeated);
    }
  }

  // (Re)set a watch on the znode.
  ret = zk->getChildren(znode, true, NULL);

  if (ret != ZOK) {
    LOG(FATAL) << "Failed to set a watch on '" << znode
               << "' in ZooKeeper: " << zk->error(ret);
  }
}


void ElectionProcess::registerCandidate(
    const PID<Candidate>& candidate,
    const string& data)
{
  int ret;
  string result;

  // Create a new ephemeral znode for this candidate and populate it
  // with the pid for this process.
  ret = zk->create(znode + "/", data, ZOO_OPEN_ACL_UNSAFE,
                   // ZOO_CREATOR_ALL_ACL, // needs authentication
                   ZOO_SEQUENCE | ZOO_EPHEMERAL, &result);

  if (ret != ZOK) {
    LOG(FATAL) << "Failed to create an ephmeral node at '"
               << znode << "' in ZooKeeper: " << zk->error(ret);
  }

  // Save the sequence id but only grab the basename, e.g.,
  // "/path/to/znode/000000131" => "000000131".
  result = utils::os::basename(result);

  uint64_t id;

  try {
    id = boost::lexical_cast<uint64_t>(result);
  } catch (boost::bad_lexical_cast&) {
    LOG(FATAL) << "Failed to convert '" << result << "' into an integer";
  }

  CandidateInfo* info = new CandidateInfo();
  info->pid = candidate;
  info->data = data;
  info->id = id;
  info->elected = false;

  infos.push_back(info);
}


void ElectionProcess::registerCandidates()
{
  while (!queue.empty()) {
    const pair<PID<Candidate>, string>& pair = queue.front();
    registerCandidate(pair.first, pair.second);
    queue.pop();
  }
}


Election::Election(const string& servers, const string& znode)
{
  process = new ElectionProcess(this, servers, znode);
  spawn(process);
}


Election::~Election()
{
  terminate(process);
  wait(process);
  delete process;
}


void Election::submit(const PID<Candidate>& candidate, const string& data)
{
  dispatch(process, &ElectionProcess::submit, candidate, data);
}


void Election::resign(const PID<Candidate>& candidate)
{
  dispatch(process, &ElectionProcess::resign, candidate);
}

}}} // namespace mesos { namespace internal { namespace zookeeper {


using namespace process;

using mesos::internal::zookeeper::Candidate;
using mesos::internal::zookeeper::Election;


class TestCandidate : public Candidate
{
public:
  virtual void onElected(Election* election)
  {
    std::cout << "TestCandidate::onElected" << std::endl;
  }

  virtual void onDefeated()
  {
    std::cout << "TestCandidate::onDefeated" << std::endl;
  }
};


int main(int argc, char** argv)
{
  if (argc != 3) {
    fatal("usage: %s <servers> <znode>", argv[0]);
  }

  string servers = argv[1];
  string znode = argv[2];

  Election election(servers, znode);

  TestCandidate candidate;
  spawn(&candidate);

  election.submit(candidate, "test");

  wait(candidate);

  return 0;
}
