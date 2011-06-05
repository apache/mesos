#ifndef __MESOS_LOCAL_HPP__
#define __MESOS_LOCAL_HPP__

#include <process.hpp>

#include "configurator/configurator.hpp"


namespace mesos { namespace internal { namespace local {

// Register the options recognized by the local runner (a combination of
// master and slave options) with a configurator.
void registerOptions(Configurator* conf);

// Launch a local cluster with a given number of slaves and given numbers
// of CPUs and memory per slave. Additionally one can also toggle whether
// to initialize Google Logging, whether to log quietly, and whether to
// include the date in master IDs (this is useful to disable for unit tests).
PID launch(int numSlaves,
           int32_t cpus,
           int64_t mem,
           bool initLogging,
           bool quiet,
           bool dateInMasterId = true);

// Launch a local cluster with a given configuration.
PID launch(const Params& conf, bool initLogging);

void shutdown();

}}}

#endif /* __MESOS_LOCAL_HPP__ */
