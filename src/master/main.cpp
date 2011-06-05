#include <libgen.h>

#include "common/logging.hpp"

#include "configurator/configurator.hpp"

#include "master.hpp"
#include "webui.hpp"

using std::cerr;
using std::endl;
using boost::lexical_cast;
using boost::bad_lexical_cast;

using namespace mesos::internal::master;


void usage(const char* progName, const Configurator& conf)
{
  cerr << "Usage: " << progName << " [--port=PORT] [--url=URL] [...]" << endl
       << endl
       << "URL (used for leader election with ZooKeeper) may be one of:" << endl
       << "  zoo://host1:port1,host2:port2,..." << endl
       << "  zoofile://file where file has one host:port pair per line" << endl
       << endl
       << "Supported options:" << endl
       << conf.getUsage();
}


int main(int argc, char **argv)
{
  Configurator conf;
  conf.addOption<string>("url", 'u', "URL used for leader election");
  conf.addOption<int>("port", 'p', "Port to listen on", 5050);
  conf.addOption<string>("ip", "IP address to listen on");
#ifdef MESOS_WEBUI
  conf.addOption<int>("webui_port", 'w', "Web UI port", 8080);
#endif
  Logging::registerOptions(&conf);
  Master::registerOptions(&conf);

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

  if (params.contains("port"))
    setenv("LIBPROCESS_PORT", params["port"].c_str(), 1);

  if (params.contains("ip"))
    setenv("LIBPROCESS_IP", params["ip"].c_str(), 1);

  string url = params.get("url", "");

  LOG(INFO) << "Build: " << BUILD_DATE << " by " << BUILD_USER;
  LOG(INFO) << "Starting Mesos master";

  if (chdir(dirname(argv[0])) != 0)
    fatalerror("Could not chdir into %s", dirname(argv[0]));

  Master *master = new Master(params);
  PID pid = Process::spawn(master);

  bool quiet = Logging::isQuiet(params);
  MasterDetector *detector = MasterDetector::create(url, pid, true, quiet);

#ifdef MESOS_WEBUI
  startMasterWebUI(pid, params);
#endif
  
  Process::wait(pid);

  MasterDetector::destroy(detector);

  return 0;
}
