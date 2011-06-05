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

static Master* currentMaster = NULL;
static vector<Slave*>* currentSlaves = NULL;

static pthread_once_t glog_initialized = PTHREAD_ONCE_INIT;


void initialize_glog() {
  google::InitGoogleLogging("nexus-local");
}

} /* namespace { */


PID run_nexus(int slaves, int32_t cpus, int64_t mem,
              bool ownLogging, bool quiet)
{
  if (currentMaster != NULL)
    fatal("Call to run_nexus while it is already running");
  
  if (ownLogging) {
    pthread_once(&glog_initialized, initialize_glog);
    if (!quiet)
      google::SetStderrLogging(google::INFO);
  }

  currentMaster = new Master();

  Process::spawn(currentMaster);

  currentSlaves = new vector<Slave*>();

  for (int i = 0; i < slaves; i++) {
    Slave* s = new Slave(currentMaster->getPID(), Resources(cpus, mem), true);
    currentSlaves->push_back(s);
    Process::spawn(s);
  }

  return currentMaster->getPID();
}


void kill_nexus()
{
  Process::post(currentMaster->getPID(), M2M_SHUTDOWN);

  Process::wait(currentMaster->getPID());
  delete currentMaster;
  currentMaster = NULL;

  for (int i = 0; i < currentSlaves->size(); i++) {
    Process::wait(currentSlaves->at(i));
    delete currentSlaves->at(i);
  }

  delete currentSlaves;
  currentSlaves = NULL;
}
