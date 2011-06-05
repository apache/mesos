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


void usage(const char* progName, const Configurator& configurator)
{
  cerr << "Usage: " << progName << " [--port=PORT] [--url=URL] [...]" << endl
       << endl
       << "URL (used for leader election with ZooKeeper) may be one of:" << endl
       << "  zoo://host1:port1,host2:port2,..." << endl
       << "  zoofile://file where file has one host:port pair per line" << endl
       << endl
       << "Supported options:" << endl
       << configurator.getUsage();
}


int main(int argc, char **argv)
{
  Configurator configurator;
  configurator.addOption<string>("url", 'u', "URL used for leader election");
  configurator.addOption<int>("port", 'p', "Port to listen on", 5050);
  configurator.addOption<string>("ip", "IP address to listen on");
#ifdef MESOS_WEBUI
  configurator.addOption<int>("webui_port", 'w', "Web UI port", 8080);
#endif
  Logging::registerOptions(&configurator);
  Master::registerOptions(&configurator);

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

  if (conf.contains("port"))
    setenv("LIBPROCESS_PORT", conf["port"].c_str(), 1);

  if (conf.contains("ip"))
    setenv("LIBPROCESS_IP", conf["ip"].c_str(), 1);

  string url = conf.get("url", "");

  LOG(INFO) << "Build: " << BUILD_DATE << " by " << BUILD_USER;
  LOG(INFO) << "Starting Mesos master";

  if (chdir(dirname(argv[0])) != 0)
    fatalerror("Could not chdir into %s", dirname(argv[0]));

  Master *master = new Master(conf);
  PID pid = Process::spawn(master);

  bool quiet = Logging::isQuiet(conf);
  MasterDetector *detector = MasterDetector::create(url, pid, true, quiet);

#ifdef MESOS_WEBUI
  startMasterWebUI(pid, conf);
#endif
  
  Process::wait(pid);

  MasterDetector::destroy(detector);

  return 0;
}
