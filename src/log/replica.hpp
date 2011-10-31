#ifndef __LOG_REPLICA_HPP__
#define __LOG_REPLICA_HPP__

#include <list>
#include <set>
#include <string>

#include <process/process.hpp>
#include <process/protobuf.hpp>

#include "common/result.hpp"
#include "common/try.hpp"

#include "messages/log.hpp"

namespace mesos {
namespace internal {
namespace log {

namespace protocol {

// Some replica protocol declarations.
extern Protocol<PromiseRequest, PromiseResponse> promise;
extern Protocol<WriteRequest, WriteResponse> write;
extern Protocol<LearnRequest, LearnResponse> learn;

} // namespace protocol {


// Forward declaration.
class ReplicaProcess;


class Replica
{
public:
  // Constructs a new replica process using specified path to a
  // directory for storing the underlying log.
  Replica(const std::string& path);
  ~Replica();

  // Returns all the actions between the specified positions, unless
  // those positions are invalid, in which case returns an error.
  process::Future<std::list<Action> > read(uint64_t from, uint64_t to);

  // Returns missing positions in the log (i.e., unlearned or holes)
  // up to the specified position.
  process::Future<std::set<uint64_t> > missing(uint64_t position);

  // Returns the beginning position of the log.
  process::Future<uint64_t> beginning();

  // Returns the last written position in the log.
  process::Future<uint64_t> ending();

  // Returns the highest implicit promise this replica has given.
  process::Future<uint64_t> promised();

  // Returns the PID associated with this replica.
  process::PID<ReplicaProcess> pid();

private:
  ReplicaProcess* process;
};

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_REPLICA_HPP__
