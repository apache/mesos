#include <getopt.h>

#include <iostream>
#include <string>

#include "configurator.hpp"
#include "logging.hpp"
#include "nexus_local.hpp"

using std::cerr;
using std::endl;
using std::string;

using namespace nexus::internal;
using nexus::internal::master::Master;
using nexus::internal::slave::Slave;


void usage(const char* programName, const Configurator& conf)
{
  cerr << "Usage: " << programName
       << " [--port=PORT] [--slaves=NUM] [--cpus=NUM] [--mem=NUM] [...]" << endl
       << endl
       << "Supported options:" << endl
       << conf.getUsage();
}


int main (int argc, char **argv)
{
  Configurator conf;
  conf.addOption<int>("port", 'p', "Port to listen on", 50010);
  conf.addOption<int>("slaves", 's', "Number of slaves", 1);
  conf.addOption<int32_t>("cpus", 'c', "CPU cores for tasks per slave", 1);
  conf.addOption<int64_t>("mem", 'm', "Memory for tasks per slave, in bytes\n",
                          1 * Gigabyte);
  Logging::registerOptions(&conf);
  Master::registerOptions(&conf);
  Slave::registerOptions(&conf);

  if (argc == 2 && string("--help") == argv[1]) {
    usage(argv[0], conf);
    exit(1);
  }

  Params params;
  try {
    conf.load(argc, argv, true);
    params = conf.getParams();
  } catch (ConfigurationException& e) {
    cerr << "Configuration error: " << e.what() << endl;
    exit(1);
  }

  Logging::init(argv[0], params);

  if (params.contains("port"))
    setenv("LIBPROCESS_PORT", params["port"].c_str(), 1);

  int slaves = params.get<int>("slaves", 1);
  int32_t cpus = params.get<int32_t>("cpus", 1);
  int64_t mem = params.get<int64_t>("mem", 1 * Gigabyte);
  bool quiet = Logging::isQuiet(params);

  const PID &master = local::launch(slaves, cpus, mem, false, quiet);

  Process::wait(master);

  return 0;
}
