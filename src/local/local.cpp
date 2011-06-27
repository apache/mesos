#include <pthread.h>

#include <map>
#include <sstream>
#include <vector>

#include "local.hpp"

#include "common/foreach.hpp"
#include "common/logging.hpp"

#include "configurator/configurator.hpp"

#include "detector/detector.hpp"

#include "master/master.hpp"

#include "slave/process_based_isolation_module.hpp"
#include "slave/slave.hpp"

using namespace mesos::internal;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;
using mesos::internal::slave::IsolationModule;
using mesos::internal::slave::ProcessBasedIsolationModule;

using process::PID;
using process::UPID;

using std::map;
using std::stringstream;
using std::vector;


namespace {

static pthread_once_t glog_initialized = PTHREAD_ONCE_INIT;


void initialize_glog() {
  google::InitGoogleLogging("mesos-local");
}

} // namespace {


namespace mesos { namespace internal { namespace local {

static Master* master = NULL;
static map<IsolationModule*, Slave*> slaves;
static MasterDetector* detector = NULL;


void registerOptions(Configurator* configurator)
{
  Logging::registerOptions(configurator);
  Master::registerOptions(configurator);
  Slave::registerOptions(configurator);
  configurator->addOption<int>("num_slaves",
                               "Number of slaves to create for local cluster",
                               1);
}


PID<Master> launch(int numSlaves,
                   int32_t cpus,
                   int64_t mem,
                   bool initLogging,
                   bool quiet)
{
  Configuration conf;
  conf.set("slaves", "*");
  conf.set("num_slaves", numSlaves);
  conf.set("quiet", quiet);

  stringstream out;
  out << "cpus:" << cpus << ";" << "mem:" << mem;
  conf.set("resources", out.str());

  return launch(conf, initLogging);
}


PID<Master> launch(const Configuration& conf,
                   bool initLogging)
{
  int numSlaves = conf.get<int>("num_slaves", 1);
  bool quiet = conf.get<bool>("quiet", false);

  if (master != NULL) {
    fatal("can only launch one local cluster at a time (for now)");
  }

  if (initLogging) {
    pthread_once(&glog_initialized, initialize_glog);
    if (!quiet) {
      google::SetStderrLogging(google::INFO);
    }
  }

  master = new Master(conf);

  PID<Master> pid = process::spawn(master);

  vector<UPID> pids;

  for (int i = 0; i < numSlaves; i++) {
    // TODO(benh): Create a local isolation module?
    ProcessBasedIsolationModule *isolationModule =
      new ProcessBasedIsolationModule();
    Slave* slave = new Slave(conf, true, isolationModule);
    slaves[isolationModule] = slave;
    pids.push_back(process::spawn(slave));
  }

  detector = new BasicMasterDetector(pid, pids, true);

  return pid;
}


void shutdown()
{
  if (master != NULL) {
    process::post(master->self(), process::TERMINATE);
    process::wait(master->self());
    delete master;
    master = NULL;

    // TODO(benh): Ugh! Because the isolation module calls back into the
    // slave (not the best design) we can't delete the slave until we
    // have deleted the isolation module. But since the slave calls into
    // the isolation module, we can't delete the isolation module until
    // we have stopped the slave.

    foreachpair (IsolationModule* isolationModule, Slave* slave, slaves) {
      process::post(slave->self(), process::TERMINATE);
      process::wait(slave->self());
      delete isolationModule;
      delete slave;
    }

    slaves.clear();

    delete detector;
    detector = NULL;
  }
}

}}} // namespace mesos { namespace internal { namespace local {
