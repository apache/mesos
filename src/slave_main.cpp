#include <getopt.h>

#include "slave.hpp"
#include "slave_webui.hpp"

using namespace std;
using namespace nexus::internal::slave;


void usage(const char *programName)
{
  cerr << "Usage: " << programName
       << " [--cpus NUM] [--mem NUM] [--isolation TYPE] [--server MASTER_PID][--fault-tolerant ZOOKEEPER_SERVER] [--quiet]"
       << endl;
}


int main(int argc, char **argv)
{
  if (argc == 2 && string("--help") == argv[1]) {
    usage(argv[0]);
    exit(1);
  }

  option options[] = {
    {"cpus", required_argument, 0, 'c'},
    {"mem", required_argument, 0, 'm'},
    {"isolation", required_argument, 0, 'i'},
    {"server", optional_argument, 0, 's'},
    {"fault-tolerant", optional_argument, 0, 'f'},
    {"quiet", no_argument, 0, 'q'},
  };

  Resources resources(1, 1 * Gigabyte);
  bool isFT = false;
  string zkserver = "";
  bool quiet = false;
  string isolation = "process";

  int opt;
  int index;
  string server="";
  bool serveropt = false;
  while ((opt = getopt_long(argc, argv, "c:m:i:f:s:q", options, &index)) != -1) {
    switch (opt) {
      case 'c':
	resources.cpus = atoi(optarg);
        break;
      case 'm':
	resources.mem = atoll(optarg);
        break;
      case 'i':
	isolation = optarg;
        break;
      case 'f':
        isFT = true;
	zkserver = optarg;
        break;
      case 's':
	serveropt = true;
	server = optarg;
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

  FLAGS_logbufsecs = 1;
  google::InitGoogleLogging(argv[0]);

  // Check that we have exactly one non-option argument (the master PID)
  // if (optind != argc - 1) {
  //   usage(argv[0]);
  //   exit(1);
  // }
  if (!isFT && !serveropt) {
    cerr<<"Must either supply MASTER_PID or ZOOKEEPER_SERVER as parameters"<<endl;
    usage(argv[0]);
    exit(1);
  }

  // Read and resolve the master PID
  PID master;

  if (serveropt) {
    LOG(INFO) << "Parsing MASTER_PID";
    istringstream iss(server);
    if (!(iss >> master)) {
      cerr << "Failed to resolve master PID " << server << endl;
      exit(1);
    }
  }

  LOG(INFO) << "Build: " << BUILD_DATE << " by " << BUILD_USER;
  LOG(INFO) << "Starting Nexus slave";
  PID slave = Process::spawn(new Slave(master, resources, false, isolation, isFT, zkserver));

#ifdef NEXUS_WEBUI
  startSlaveWebUI(slave);
#endif

  Process::wait(slave);
  return 0;
}
