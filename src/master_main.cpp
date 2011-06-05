#include <getopt.h>
#include <libgen.h>

#include "master.hpp"
#include "master_webui.hpp"

using std::cerr;
using std::endl;

using namespace nexus::internal::master;


void usage(const char* programName)
{
  cerr << "Usage: " << programName
       << " [--url URL]"
       << " [--port PORT]"
       << " [--allocator ALLOCATOR]"
       << " [--quiet]" << endl
       << endl
       << "URL (used for contending to be a master) may be one of:" << endl
       << "  zoo://host1:port1,host2:port2,..." << endl
       << "  zoofile://file where file contains a host:port pair per line"
       << endl;
}


int main (int argc, char **argv)
{
  if (argc == 2 && string("--help") == argv[1]) {
    usage(argv[0]);
    exit(1);
  }

  option options[] = {
    {"url", required_argument, 0, 'u'},
    {"allocator", required_argument, 0, 'a'},
    {"port", required_argument, 0, 'p'},
    {"quiet", no_argument, 0, 'q'},
  };

  string url = "";
  string allocator = "simple";
  bool quiet = false;

  int opt;
  int index;
  while ((opt = getopt_long(argc, argv, "u:a:p:q", options, &index)) != -1) {
    switch (opt) {
      case 'u':
        url = optarg;
        break;
      case 'a':
        allocator = optarg;
        break;
      case 'p':
        setenv("LIBPROCESS_PORT", optarg, 1);
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

  // TODO(benh): Don't log to /tmp behind a sys admin's back!
  FLAGS_log_dir = "/tmp";
  FLAGS_logbufsecs = 1;
  google::InitGoogleLogging(argv[0]);

  if (!quiet)
    google::SetStderrLogging(google::INFO);

  LOG(INFO) << "Build: " << BUILD_DATE << " by " << BUILD_USER;
  LOG(INFO) << "Starting Nexus master";

  Master *master = new Master(allocator);
  PID pid = Process::spawn(master);

  MasterDetector *detector = MasterDetector::create(url, pid, true, quiet);

#ifdef NEXUS_WEBUI
  if (chdir(dirname(argv[0])) != 0)
    fatalerror("could not change into %s for running webui", dirname(argv[0]));
  startMasterWebUI(pid);
#endif
  
  Process::wait(pid);

  MasterDetector::destroy(detector);

  return 0;
}
