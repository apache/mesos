#include <pthread.h>

#include <map>
#include <sstream>
#include <vector>

#include "local.hpp"

#include "common/foreach.hpp"
#include "common/logging.hpp"

#include "configurator/configurator.hpp"

#include "master/master.hpp"

#include "slave/process_based_isolation_module.hpp"
#include "slave/slave.hpp"

using namespace mesos::internal;

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;
using mesos::internal::slave::IsolationModule;
using mesos::internal::slave::ProcessBasedIsolationModule;

using std::map;
using std::stringstream;
using std::vector;


namespace {

static pthread_once_t glog_initialized = PTHREAD_ONCE_INIT;


void initialize_glog() {
  google::InitGoogleLogging("mesos-local");
}

} /* namespace { */


namespace mesos { namespace internal { namespace local {

static Master *master = NULL;
static map<IsolationModule*, Slave*> slaves;
static MasterDetector *detector = NULL;


void registerOptions(Configurator* configurator)
{
  configurator->addOption<int>("slaves", 's', "Number of slaves", 1);
  Logging::registerOptions(configurator);
  Master::registerOptions(configurator);
  Slave::registerOptions(configurator);
}


PID launch(int numSlaves,
           int32_t cpus,
           int64_t mem,
           bool initLogging,
           bool quiet)
{
  Configuration conf;
  conf.set("slaves", numSlaves);
  conf.set("quiet", quiet);

  stringstream out;
  out << "cpus:" << cpus << ";" << "mem:" << mem;
  conf.set("resources", out.str());

  return launch(conf, initLogging);
}


PID launch(const Configuration& conf, bool initLogging)
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

  master = new Master(conf);

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
  if (master != NULL) {
    Process::post(master->self(), TERMINATE);
    Process::wait(master->self());
    delete master;
    master = NULL;

    // TODO(benh): Ugh! Because the isolation module calls back into the
    // slave (not the best design) we can't delete the slave until we
    // have deleted the isolation module. But since the slave calls into
    // the isolation module, we can't delete the isolation module until
    // we have stopped the slave.

    foreachpair (IsolationModule *isolationModule, Slave *slave, slaves) {
      Process::post(slave->self(), TERMINATE);
      Process::wait(slave->self());
      delete isolationModule;
      delete slave;
    }

    slaves.clear();

    delete detector;
    detector = NULL;
  }
}

}}} /* namespace mesos { namespace internal { namespace local { */
