#include <getopt.h>
#include <libgen.h>

#include "configurator.hpp"
#include "master.hpp"
#include "master_webui.hpp"

using std::cerr;
using std::endl;
using boost::lexical_cast;
using boost::bad_lexical_cast;

using namespace nexus::internal::master;


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
  conf.addOption<int>("port", 'p', "Port to listen on", 50010);
  conf.addOption<bool>("quiet", 'q', "Disable logging to stderr", false);
  conf.addOption<string>("log_dir", "Where to place logs", "/tmp");
#ifdef NEXUS_WEBUI
  conf.addOption<int>("webui_port", 'w', "Web UI port", 8080);
#endif
  Master::registerOptions(&conf);

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

  if (params.contains("port"))
    setenv("LIBPROCESS_PORT", params["port"].c_str(), 1);

  FLAGS_log_dir = params["log_dir"];
  FLAGS_logbufsecs = 1;
  google::InitGoogleLogging(argv[0]);

  bool quiet = params.get<bool>("quiet", false);
  if (!quiet)
    google::SetStderrLogging(google::INFO);

  string url = params.get("url", "");

  LOG(INFO) << "Build: " << BUILD_DATE << " by " << BUILD_USER;
  LOG(INFO) << "Starting Nexus master";

  if (chdir(dirname(argv[0])) != 0)
    fatalerror("Could not chdir into %s", dirname(argv[0]));

  Master *master = new Master(params);
  PID pid = Process::spawn(master);

  MasterDetector *detector = MasterDetector::create(url, pid, true, quiet);

#ifdef NEXUS_WEBUI
  startMasterWebUI(pid, (char*) params["webui_port"].c_str());
#endif
  
  Process::wait(pid);

  MasterDetector::destroy(detector);

  return 0;
}
