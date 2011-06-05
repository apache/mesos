#include "common/logging.hpp"

#include "configurator/configurator.hpp"

#include "isolation_module_factory.hpp"
#include "slave.hpp"
#include "webui.hpp"

using boost::lexical_cast;
using boost::bad_lexical_cast;

using namespace std;

using namespace mesos::internal::slave;


void usage(const char *programName, const Configurator& conf)
{
  cerr << "Usage: " << programName
       << " --url=MASTER_URL [--cpus=NUM] [--mem=BYTES] [...]" << endl
       << endl
       << "MASTER_URL may be one of:" << endl
       << "  mesos://id@host:port" << endl
       << "  zoo://host1:port1,host2:port2,..." << endl
       << "  zoofile://file where file contains a host:port pair per line"
       << endl
       << endl
       << "Supported options:" << endl
       << conf.getUsage();
}


int main(int argc, char **argv)
{
  Configurator conf;
  conf.addOption<string>("url", 'u', "Master URL");
  conf.addOption<string>("isolation", 'i', "Isolation module name", "process");
#ifdef MESOS_WEBUI
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
    params = conf.load(argc, argv, true);
  } catch (ConfigurationException& e) {
    cerr << "Configuration error: " << e.what() << endl;
    exit(1);
  }

  Logging::init(argv[0], params);

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
  LOG(INFO) << "Starting Mesos slave";

  if (chdir(dirname(argv[0])) != 0)
    fatalerror("Could not chdir into %s", dirname(argv[0]));

  Slave* slave = new Slave(params, false, isolationModule);
  PID pid = Process::spawn(slave);

  bool quiet = Logging::isQuiet(params);
  MasterDetector *detector = MasterDetector::create(url, pid, false, quiet);

#ifdef MESOS_WEBUI
  startSlaveWebUI(pid, params);
#endif

  Process::wait(pid);

  MasterDetector::destroy(detector);

  IsolationModule::destroy(isolationModule);

  delete slave;

  return 0;
}
