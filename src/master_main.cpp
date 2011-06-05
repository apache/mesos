#include <getopt.h>
#include <libgen.h>

#include "master.hpp"
#include "master_webui.hpp"
#include "url_processor.hpp"

using std::cerr;
using std::endl;

using namespace nexus::internal::master;


void usage(const char* programName)
{
  cerr << "Usage: " << programName
       << " [--port PORT]"
       << " [--allocator ALLOCATOR]"
       << " [--zookeeper ZOO_SERVERS]"
       << " [--quiet]" << endl
       << endl
       << "ZOO_SERVERS is a url of the form: "
       << "zoo://host1:port1,host2:port2,... or "
       << "zoofile://file where file contains a host:port pair per line"
       << endl;
}


int main (int argc, char **argv)
{
  if (argc == 2 && string("--help") == argv[1]) {
    usage(argv[0]);
    exit(1);
  }

  option options[] = {
    {"allocator", required_argument, 0, 'a'},
    {"port", required_argument, 0, 'p'},
    {"zookeeper", required_argument, 0, 'z'},
    {"quiet", no_argument, 0, 'q'},
  };

  bool isFT = false;
  string zookeeper = "";
  bool quiet = false;
  string allocator = "simple";

  int opt;
  int index;
  while ((opt = getopt_long(argc, argv, "a:p:z:q", options, &index)) != -1) {
    switch (opt) {
      case 'a':
        allocator = optarg;
        break;
      case 'p':
        setenv("LIBPROCESS_PORT", optarg, 1);
        break;
      case 'z':
	isFT = true;
        zookeeper = optarg;
	break;
      case 'q':
        quiet = true;
        break;
      case '?':
        // Error parsing options; getopt prints an error message, so just exit
        exit(1);
        break;
      default:
        break;
    }
  }

  if (!quiet)
    google::SetStderrLogging(google::INFO);
  else if (isFT)
    MasterDetector::setQuiet(true);

  FLAGS_log_dir = "/tmp";
  FLAGS_logbufsecs = 1;
  google::InitGoogleLogging(argv[0]);

  LOG(INFO) << "Build: " << BUILD_DATE << " by " << BUILD_USER;
  LOG(INFO) << "Starting Nexus master";
  if (isFT)
    LOG(INFO) << "Nexus in fault-tolerant mode";
  Master *master = new Master(allocator, zookeeper);
  PID pid = Process::spawn(master);

#ifdef NEXUS_WEBUI
  if (chdir(dirname(argv[0])) != 0)
    fatalerror("could not change into %s for running webui", dirname(argv[0]));
  startMasterWebUI(pid);
#endif
  
  Process::wait(pid);
  return 0;
}
