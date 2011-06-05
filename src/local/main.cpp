#include <iostream>
#include <string>

#include "local.hpp"

#include "configurator/configurator.hpp"

#include "detector/detector.hpp"

#include "common/logging.hpp"

#include "master/master.hpp"

#include "slave/slave.hpp"

using namespace mesos::internal;

using mesos::internal::master::Master;
using mesos::internal::slave::Slave;

using std::cerr;
using std::endl;
using std::string;


void usage(const char* programName, const Configurator& configurator)
{
  cerr << "Usage: " << programName
       << " [--port=PORT] [--slaves=N] [--cpus=CPUS] [--mem=MEM] [...]" << endl
       << endl
       << "Launches a single-process cluster containing N slaves, each of "
       << "which report" << endl << "CPUS cores and MEM bytes of memory."
       << endl
       << endl
       << "Supported options:" << endl
       << configurator.getUsage();
}


int main(int argc, char **argv)
{
  Configurator configurator;
  Logging::registerOptions(&configurator);
  local::registerOptions(&configurator);
  configurator.addOption<int>("port", 'p', "Port to listen on", 5050);
  configurator.addOption<string>("ip", "IP address to listen on");

  if (argc == 2 && string("--help") == argv[1]) {
    usage(argv[0], configurator);
    exit(1);
  }

  Configuration conf;
  try {
    conf = configurator.load(argc, argv, true);
  } catch (ConfigurationException& e) {
    cerr << "Configuration error: " << e.what() << endl;
    exit(1);
  }

  Logging::init(argv[0], conf);

  if (conf.contains("port")) {
    setenv("LIBPROCESS_PORT", conf["port"].c_str(), 1);
  }

  if (conf.contains("ip")) {
    setenv("LIBPROCESS_IP", conf["ip"].c_str(), 1);
  }

  // Initialize libprocess library (but not glog, done above).
  process::initialize(false);

  process::wait(local::launch(conf, false));

  return 0;
}
