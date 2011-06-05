#include <getopt.h>
#include <libgen.h>

#include "master.hpp"
#include "master_webui.hpp"

using std::cerr;
using std::endl;

using namespace nexus::internal::master;


/* List of ZooKeeper host:port pairs. */
string zookeeper = "";


void usage(const char* programName)
{
  cerr << "Usage: " << programName
       << " [--port PORT]"
       << " [--allocator ALLOCATOR]"
       << " [--zookeeper host:port]"
       << " [--quiet]"
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
#ifndef USING_ZOOKEEPER
	cerr << "--zookeeper not supported in this build" << endl;
	usage(argv[0]);
	exit(1);
#else
        zookeeper = optarg;
#endif
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
  
  FLAGS_log_dir = "/tmp";
  FLAGS_logbufsecs = 1;
  google::InitGoogleLogging(argv[0]);

  LOG(INFO) << "Build: " << BUILD_DATE << " by " << BUILD_USER;
  LOG(INFO) << "Starting Nexus master";

  Master* master = new Master(allocator);
  PID pid = Process::spawn(master);

#ifdef NEXUS_WEBUI
  if (chdir(dirname(argv[0])) != 0)
    fatalerror("could not change into %s for running webui", dirname(argv[0]));
  startMasterWebUI(pid);
#endif
  
  Process::wait(pid);
  return 0;
}
