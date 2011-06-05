#include <getopt.h>

#include "configurator.hpp"
#include "isolation_module_factory.hpp"
#include "logging.hpp"
#include "slave.hpp"
#include "slave_webui.hpp"

using boost::lexical_cast;
using boost::bad_lexical_cast;

using namespace std;

using namespace nexus::internal::slave;


void usage(const char *programName, const Configurator& conf)
{
  cerr << "Usage: " << programName
       << " --url=MASTER_URL [--cpus=NUM] [--mem=BYTES] [...]" << endl
       << endl
       << "MASTER_URL may be one of:" << endl
       << "  nexus://id@host:port" << endl
       << "  zoo://host1:port1,host2:port2,..." << endl
       << "  zoofile://file where file contains a host:port pair per line"
       << endl
       << conf.getUsage();
}


int main(int argc, char **argv)
{
  Configurator conf;
  conf.addOption<string>("url", 'u', "Master URL");
  conf.addOption<int>("port", 'p', "Port to listen on (default: random)");
  conf.addOption<string>("isolation", 'i', "Isolation module name", "process");
  conf.addOption<int32_t>("cpus", 'c', "CPU cores to use for tasks", 1);
  conf.addOption<int64_t>("mem", 'm', "Memory to use for tasks, in bytes\n",
                          1 * Gigabyte);
#ifdef NEXUS_WEBUI
  conf.addOption<int>("webui_port", 'w', "Web UI port", 8081);
#endif
  Logging::registerOptions(&conf);
  Slave::registerOptions(&conf);

  if (argc == 2 && string("--help") == argv[1]) {
    usage(argv[0], conf);
    exit(1);
  }

  Params params;
  try {
    conf.load(argc, argv, true);
    params = conf.getParams();
  } catch (BadOptionValueException& e) {
    cerr << "Invalid value for '" << e.what() << "' option" << endl;
    exit(1);
  } catch (ConfigurationException& e) {
    cerr << "Configuration error: " << e.what() << endl;
    exit(1);
  }

  Logging::init(argv[0], params);

  if (params.contains("port"))
    setenv("LIBPROCESS_PORT", params["port"].c_str(), 1);

  if (!params.contains("url")) {
    cerr << "Master URL argument (--url) required." << endl;
    exit(1);
  }
  string url = params["url"];

  string isolation = params["isolation"];
  LOG(INFO) << "Creating \"" << isolation << "\" isolation module";
  IsolationModule *isolationModule = IsolationModule::create(isolation);

  if (isolationModule == NULL) {
    cerr << "Unrecognized isolation type: " << isolation << endl;
    exit(1);
  }

  LOG(INFO) << "Build: " << BUILD_DATE << " by " << BUILD_USER;
  LOG(INFO) << "Starting Nexus slave";

  if (chdir(dirname(argv[0])) != 0)
    fatalerror("Could not chdir into %s", dirname(argv[0]));

  Resources resources(params.get<int32_t>("cpus", 1),
                      params.get<int64_t>("mem", 1 * Gigabyte));
  Slave* slave = new Slave(params, resources, false, isolationModule);
  PID pid = Process::spawn(slave);

  bool quiet = Logging::isQuiet(params);
  MasterDetector *detector = MasterDetector::create(url, pid, false, quiet);

#ifdef NEXUS_WEBUI
  startSlaveWebUI(pid, (char*) params["webui_port"].c_str());
#endif

  Process::wait(pid);

  MasterDetector::destroy(detector);

  IsolationModule::destroy(isolationModule);

  delete slave;

  return 0;
}
