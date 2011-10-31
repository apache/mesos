#ifndef __LOG_COORDINATOR_HPP__
#define __LOG_COORDINATOR_HPP__

#include <string>
#include <vector>

#include <process/process.hpp>

#include "common/result.hpp"

#include "log/network.hpp"
#include "log/replica.hpp"

#include "messages/log.hpp"


namespace mesos {
namespace internal {
namespace log {

using namespace process;

// TODO(benh): Pass timeouts into the coordinator functions rather
// than have hard coded timeouts within.

class Coordinator
{
public:
  Coordinator(int quorum,
              Replica* replica,
              Network* group);

  ~Coordinator();

  // Handles coordinator election/demotion. A result of none means the
  // coordinator failed to achieve a quorum (e.g., due to timeout) but
  // can be retried. A some result returns the last committed log
  // position.
  Result<uint64_t> elect();
  Result<uint64_t> demote();

  // Returns the result of trying to append the specified bytes. A
  // result of none means the append failed (e.g., due to timeout),
  // but can be retried.
  Result<uint64_t> append(const std::string& bytes);

  // Returns the result of trying to truncate the log (from the
  // beginning to the specified position exclusive). A result of
  // none means the truncate failed (e.g., due to timeout), but can be
  // retried.
  Result<uint64_t> truncate(uint64_t to);

private:
  // Helper that tries to achieve consensus of the specified action. A
  // result of none means the write failed (e.g., due to timeout), but
  // can be retried.
  Result<uint64_t> write(const Action& action);

  // Helper that handles commiting an action (i.e., writing to the
  // local replica and then sending out learned messages).
  Result<uint64_t> commit(const Action& action);

  // Helper that tries to fill a position in the log.
  Result<Action> fill(uint64_t position);

  // Helper that uses the specified protocol to broadcast a request to
  // our group and return a set of futures.
  template <typename Req, typename Res>
  std::set<Future<Res> > broadcast(
      const Protocol<Req, Res>& protocol,
      const Req& req);

  // Helper like broadcast, but excludes our local replica.
  template <typename Req, typename Res>
  std::set<Future<Res> > remotecast(
      const Protocol<Req, Res>& protocol,
      const Req& req);

  // Helper like remotecast but ignores any responses.
  template <typename M>
  void remotecast(const M& m);

  bool elected; // True if this coordinator has been elected.

  int quorum; // Quorum size.

  Replica* replica; // Local log replica.

  Network* network; // Used to broadcast requests and messages to replicas.

  uint64_t id; // Coordinator ID.

  uint64_t index; // Last position written in the log.
};

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_COORDINATOR_HPP__
