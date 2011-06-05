#include <getopt.h>
#include <pthread.h>

#include <tuple.hpp>

#include <map>
#include <vector>

#include "configuration.hpp"
#include "foreach.hpp"
#include "nexus_local.hpp"
#include "process_based_isolation_module.hpp"

using std::map;
using std::vector;

using nexus::internal::master::Master;
using nexus::internal::slave::Slave;
using nexus::internal::slave::IsolationModule;
using nexus::internal::slave::ProcessBasedIsolationModule;

using namespace nexus::internal;


namespace {

static pthread_once_t glog_initialized = PTHREAD_ONCE_INIT;


void initialize_glog() {
  google::InitGoogleLogging("nexus-local");
}

} /* namespace { */


namespace nexus { namespace internal { namespace local {

static Master *master = NULL;
static map<IsolationModule*, Slave*> slaves;
static MasterDetector *detector = NULL;


PID launch(int numSlaves, int32_t cpus, int64_t mem,
	   bool initLogging, bool quiet)
{
  if (master != NULL)
    fatal("can only launch one local cluster at a time (for now)");

  if (initLogging) {
    pthread_once(&glog_initialized, initialize_glog);
    if (!quiet)
      google::SetStderrLogging(google::INFO);
  }

  Params conf;
  master = new Master(conf);

  PID pid = Process::spawn(master);

  vector<PID> pids;

  for (int i = 0; i < numSlaves; i++) {
    // TODO(benh): Create a local isolation module?
    ProcessBasedIsolationModule *isolationModule =
      new ProcessBasedIsolationModule();
    Slave* slave = new Slave(Resources(cpus, mem), true, isolationModule);
    slaves[isolationModule] = slave;
    pids.push_back(Process::spawn(slave));
  }

  detector = new BasicMasterDetector(pid, pids, true);

  return pid;
}


void shutdown()
{
  Process::post(master->getPID(), M2M_SHUTDOWN);
  Process::wait(master->getPID());
  delete master;
  master = NULL;

  // TODO(benh): Ugh! Because the isolation module calls back into the
  // slave (not the best design) we can't delete the slave until we
  // have deleted the isolation module. But since the slave calls into
  // the isolation module, we can't delete the isolation module until
  // we have stopped the slave.

  foreachpair (IsolationModule *isolationModule, Slave *slave, slaves) {
    Process::post(slave->getPID(), S2S_SHUTDOWN);
    Process::wait(slave);
    delete isolationModule;
    delete slave;
  }

  slaves.clear();

  delete detector;
  detector = NULL;
}

}}} /* namespace nexus { namespace internal { namespace local { */
