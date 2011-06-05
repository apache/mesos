#include <pthread.h>

#include <map>
#include <vector>

#include "local.hpp"

#include "common/foreach.hpp"
#include "common/logging.hpp"

#include "configurator/configurator.hpp"

#include "event_history/event_logger.hpp"
 
#include "master/master.hpp"

#include "slave/process_based_isolation_module.hpp"
#include "slave/slave.hpp"

using std::map;
using std::vector;

using mesos::internal::eventhistory::EventLogger;
using mesos::internal::master::Master;
using mesos::internal::slave::Slave;
using mesos::internal::slave::IsolationModule;
using mesos::internal::slave::ProcessBasedIsolationModule;

using namespace mesos::internal;


namespace {

static pthread_once_t glog_initialized = PTHREAD_ONCE_INIT;


void initialize_glog() {
  google::InitGoogleLogging("mesos-local");
}

} /* namespace { */


namespace mesos { namespace internal { namespace local {

static EventLogger* evLogger = NULL;
static Master *master = NULL;
static map<IsolationModule*, Slave*> slaves;
static MasterDetector *detector = NULL;


void registerOptions(Configurator* conf)
{
  conf->addOption<int>("slaves", 's', "Number of slaves", 1);
  EventLogger::registerOptions(conf);
  Logging::registerOptions(conf);
  Master::registerOptions(conf);
  Slave::registerOptions(conf);
}


PID launch(int numSlaves,
           int32_t cpus,
           int64_t mem,
           bool initLogging,
           bool quiet)
{
  Params conf;
  conf.set("slaves", numSlaves);
  conf.set("cpus", cpus);
  conf.set("mem", mem);
  conf.set("quiet", quiet);
  return launch(conf, initLogging);
}


PID launch(const Params& conf, bool initLogging)
{
  int numSlaves = conf.get<int>("slaves", 1);
  bool quiet = conf.get<bool>("quiet", false);

  if (master != NULL)
    fatal("can only launch one local cluster at a time (for now)");

  if (initLogging) {
    pthread_once(&glog_initialized, initialize_glog);
    if (!quiet)
      google::SetStderrLogging(google::INFO);
  }

  evLogger = new EventLogger(conf);

  master = new Master(conf, evLogger);

  PID pid = Process::spawn(master);

  vector<PID> pids;

  for (int i = 0; i < numSlaves; i++) {
    // TODO(benh): Create a local isolation module?
    ProcessBasedIsolationModule *isolationModule =
      new ProcessBasedIsolationModule();
    Slave* slave = new Slave(conf, true, isolationModule);
    slaves[isolationModule] = slave;
    pids.push_back(Process::spawn(slave));
  }

  detector = new BasicMasterDetector(pid, pids, true);

  return pid;
}


void shutdown()
{
  MesosProcess::post(master->self(), pack<M2M_SHUTDOWN>());
  Process::wait(master->self());
  delete master;
  delete evLogger;
  master = NULL;

  // TODO(benh): Ugh! Because the isolation module calls back into the
  // slave (not the best design) we can't delete the slave until we
  // have deleted the isolation module. But since the slave calls into
  // the isolation module, we can't delete the isolation module until
  // we have stopped the slave.

  foreachpair (IsolationModule *isolationModule, Slave *slave, slaves) {
    MesosProcess::post(slave->self(), pack<S2S_SHUTDOWN>());
    Process::wait(slave->self());
    delete isolationModule;
    delete slave;
  }

  slaves.clear();

  delete detector;
  detector = NULL;
}

}}} /* namespace mesos { namespace internal { namespace local { */
