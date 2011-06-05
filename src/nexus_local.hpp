#ifndef LOCAL_HPP
#define LOCAL_HPP

// Include the master and slave headers here so that the nexus_sched
// library will re-compile when they are changed.
#include "master.hpp"
#include "slave.hpp"

PID run_nexus(int slaves, int32_t cpus, int64_t mem,
              bool ownLogging, bool quiet);
void kill_nexus();

#endif /* LOCAL_HPP */
