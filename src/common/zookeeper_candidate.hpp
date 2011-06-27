#include <process/process.hpp>


namespace mesos { namespace internal { namespace zookeeper {

using namespace process;

class Election;
class ElectionProcess;


// Interface definition for becoming a ZooKeeper-based group leader.
class Candidate : public Process<Candidate>
{
public:
  // Called when this leader has been elected.
  virtual void onElected(Election* election) = 0;

  // Called when the leader has been ousted.  Can occur either if the
  // leader abdicates or if an external event causes the leader to
  // lose its leadership role (session expiration).
  virtual void onDefeated() = 0;
};


// Performs leader election for a set of candidates using ZooKeeper.
class Election
{
public:
  Election(const std::string& servers, const std::string& znode);
  ~Election();

  // Submit candidate for election.
  void submit(const PID<Candidate>& candidate, const std::string& data);

  // Relinquish current leadership (if leader), then re-run election
  // (possibly being re-elected).
  void resign(const PID<Candidate>& candidate);

private:
  // Not copyable or assignable.
  Election(const Election& that);
  Election& operator = (const Election& that);

  // Underlying process that interacts with ZooKeeper, performs
  // elections, etc.
  ElectionProcess* process;
};

}}} // namespace mesos { namespace internal { namespace zookeeper {
