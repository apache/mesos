#include <getopt.h>

#include <iostream>
#include <string>

#include "nexus_local.hpp"

using std::cerr;
using std::endl;
using std::string;

using namespace nexus::internal;


int main (int argc, char **argv)
{
  if (argc == 2 && string("--help") == argv[1]) {
    cerr << "Usage: " << argv[0]
         << " [--port PORT] [--slaves NUM] [--cpus NUM] [--mem NUM] [--quiet]"
         << endl;
    exit(1);
  }

  int slaves = 1;
  int32_t cpus = 1;
  int64_t mem = 1073741824;
  bool quiet = false;

  option options[] = {
    {"slaves", required_argument, 0, 's'},
    {"cpus", required_argument, 0, 'c'},
    {"mem", required_argument, 0, 'm'},
    {"port", required_argument, 0, 'p'},
    {"quiet", no_argument, 0, 'q'},
  };

  int opt;
  int index;
  while ((opt = getopt_long(argc, argv, "s:c:m:p:q", options, &index)) != -1) {
    switch (opt) {
      case 's':
        slaves = atoi(optarg);
        break;
      case 'c':
        cpus = atoi(optarg);
        break;
      case 'm':
        mem = atoll(optarg);
        break;
      case 'p':
        setenv("LIBPROCESS_PORT", optarg, true);
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

  const PID &master = local::launch(slaves, cpus, mem, true, quiet);

  Process::wait(master);

  return 0;
}
