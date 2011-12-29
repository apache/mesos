#ifndef __MASTER_CONSTANTS_HPP__
#define __MASTER_CONSTANTS_HPP__

namespace mesos {
namespace internal {
namespace master {

// TODO(benh): Add units after constants.
// TODO(benh): Also make configuration options be constants.

// Maximum number of slot offers to have outstanding for each framework.
const int MAX_OFFERS_PER_FRAMEWORK = 50;

// Seconds until unused resources are re-offered to a framework.
const double UNUSED_RESOURCES_TIMEOUT = 5.0;

// Minimum number of cpus / task.
const int32_t MIN_CPUS = 1;

// Minimum amount of memory / task.
const int32_t MIN_MEM = 32 * Megabyte;

// Maximum number of CPUs per machine.
const int32_t MAX_CPUS = 1000 * 1000;

// Maximum amount of memory / machine.
const int32_t MAX_MEM = 1024 * 1024 * Megabyte;

// Acceptable timeout for slave PONG.
const double SLAVE_PONG_TIMEOUT = 15.0;

// Maximum number of timeouts until slave is considered failed.
const int MAX_SLAVE_TIMEOUTS = 5;

// Default time to wait for a framework scheduler to failover.
const int DEFAULT_FAILOVER_TIMEOUT = 1;

// Maximum number of completed frameworks to store in the cache.
// TODO(thomasm): make configurable
const int MAX_COMPLETED_FRAMEWORKS = 100;

// Maximum number of completed tasks per framework to store in the cache.
// TODO(thomasm): make configurable
const int MAX_COMPLETED_TASKS_PER_FRAMEWORK = 500;

} // namespace mesos {
} // namespace internal {
} // namespace master {

#endif // __MASTER_CONSTANTS_HPP__
