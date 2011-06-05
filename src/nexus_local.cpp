#include <getopt.h>
#include <pthread.h>

#include <tuple.hpp>

#include <vector>

#include "nexus_local.hpp"

using std::vector;

using nexus::internal::master::Master;
using nexus::internal::slave::Slave;

using namespace nexus::internal;


namespace {

static pthread_once_t glog_initialized = PTHREAD_ONCE_INIT;


void initialize_glog() {
  google::InitGoogleLogging("nexus-local");
}

} /* namespace { */


namespace nexus { namespace internal { namespace local {

static Master *master = NULL;
static vector<Slave*> *slaves = NULL;
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

  master = new Master();
  slaves = new vector<Slave*>();

  PID pid = Process::spawn(master);

  vector<PID> pids;

  for (int i = 0; i < numSlaves; i++) {
    Slave* slave = new Slave(Resources(cpus, mem), true);
    slaves->push_back(slave);
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

  for (int i = 0; i < slaves->size(); i++) {
    Process::wait(slaves->at(i));
    delete slaves->at(i);
  }

  delete slaves;
  slaves = NULL;

  delete detector;
  detector = NULL;
}

}}} /* namespace nexus { namespace internal { namespace local { */
