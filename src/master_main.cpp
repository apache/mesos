#include <getopt.h>
#include <libgen.h>

#include "configuration.hpp"
#include "master.hpp"
#include "master_webui.hpp"

using std::cerr;
using std::endl;

using namespace nexus::internal::master;


void usage(const char* programName, const Configuration& conf)
{
  cerr << "Usage: " << programName
       << " [--port PORT]"
       << " [--url URL]"
       << " [--allocator ALLOCATOR]"
       << " [--quiet]" << endl
       << endl
       << "URL (used for contending to be a master) may be one of:" << endl
       << "  zoo://host1:port1,host2:port2,..." << endl
       << "  zoofile://file where file contains a host:port pair per line"
       << endl << endl
       << "Option details:" << endl
       << conf.getUsage() << endl;
}


int main(int argc, char **argv)
{
  Configuration conf;
  conf.addOption<string>("url", "URL used for contending to a master.");
  conf.addOption<int>("port", "Port to listen on", 'p');
  conf.addOption<bool>("quiet", "Do not log to stderr", "0", 'q');
  conf.addOption<string>("log_dir", "Where to place logs", "/tmp");
  Master::registerOptions(&conf);

  if (argc == 2 && string("--help") == argv[1]) {
    usage(argv[0], conf);
    exit(1);
  }

  Params params;

  try {
    conf.load(argc, argv, true);
    conf.validate();
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
  startMasterWebUI(pid);
#endif
  
  Process::wait(pid);

  MasterDetector::destroy(detector);

  return 0;
}
