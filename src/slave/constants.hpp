#ifndef __SLAVE_CONSTANTS_HPP__
#define __SLAVE_CONSTANTS_HPP__

namespace mesos {
namespace internal {
namespace slave {

// TODO(benh): Also make configuration options be constants.

const double EXECUTOR_SHUTDOWN_TIMEOUT_SECONDS = 5.0;
const double STATUS_UPDATE_RETRY_INTERVAL_SECONDS = 10.0;

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CONSTANTS_HPP__
