#ifndef LOCAL_HPP
#define LOCAL_HPP

// Include the master and slave headers here so that the nexus_sched
// library will re-compile when they are changed.
#include "master.hpp"
#include "slave.hpp"

#include "params.hpp"

namespace nexus { namespace internal { namespace local {

PID launch(int numSlaves, int32_t cpus, int64_t mem,
	   bool initLogging, bool quiet);

PID launch(const Params& conf, bool initLogging);

void shutdown();

}}}

#endif /* LOCAL_HPP */
