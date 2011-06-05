#include <getopt.h>

#include "master.hpp"
#include "master_webui.hpp"
#include "url_processor.hpp"
#include "leader_detector.hpp"

using std::cerr;
using std::endl;
using namespace nexus::internal::master;


void usage(const char* programName)
{
  cerr << "Usage: " << programName
       << " [--port PORT] [--allocator ALLOCATOR] [--fault-tolerant ZOO_SERVERS] [--quiet]"
       << endl
       << "ZOO_SERVERS is a url of the form zoo://<zoosrv1>,<zoosrv2>..., or zoofile://listfile"
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
    {"fault-tolerant", required_argument, 0, 'f'},
    {"quiet", no_argument, 0, 'q'},
  };

  bool isFT = false;
  string zooarg = "";
  bool quiet = false;
  string allocator = "simple";

  int opt;
  int index;
  while ((opt = getopt_long(argc, argv, "a:p:f:q", options, &index)) != -1) {
    switch (opt) {
      case 'a':
        allocator = optarg;
        break;
      case 'p':
        setenv("LIBPROCESS_PORT", optarg, 1);
        break;
      case 'f':
        isFT = true;
	zooarg = optarg;
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
    LeaderDetector::setQuiet(true);

  FLAGS_log_dir = "/tmp";
  FLAGS_logbufsecs = 1;
  google::InitGoogleLogging(argv[0]);

  LOG(INFO) << "Build: " << BUILD_DATE << " by " << BUILD_USER;
  LOG(INFO) << "Starting Nexus master";
  if (isFT)
    LOG(INFO) << "Nexus in fault-tolerant mode";
  PID master = Process::spawn(new Master(allocator, zooarg));

#undef NEXUS_WEBUI
#ifdef NEXUS_WEBUI
  startMasterWebUI(master);
#endif
  
  Process::wait(master);
  return 0;
}
