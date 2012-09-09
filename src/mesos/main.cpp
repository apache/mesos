#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/os.hpp>

#include "configurator/configurator.hpp"

#include "messages/messages.hpp"

using namespace mesos::internal;

using namespace process;

using std::cerr;
using std::cout;
using std::endl;
using std::string;


void usage(const char* argv0, const Configurator& configurator)
{
  cerr << "Usage: " << os::basename(argv0) << " [...]" << endl
       << endl
       << "Supported options:" << endl
       << configurator.getUsage();
}


int main(int argc, char** argv)
{
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  Configurator configurator;

  // The following options are executable specific (e.g., since we
  // only have one instance of libprocess per execution, we only want
  // to advertise the port and ip option once, here).
  configurator.addOption<int>("port", 'p', "Port to listen on", 5050);
  configurator.addOption<string>("ip", "IP address to listen on");
#ifdef MESOS_WEBUI
  configurator.addOption<int>("webui_port", "Web UI port", 8080);
#endif
  configurator.addOption<string>(
      "master",
      'm',
      "May be one of:\n"
      "  host:port\n"
      "  zk://host1:port1,host2:port2,.../path\n"
      "  zk://username:password@host1:port1,host2:port2,.../path\n"
      "  file://path/to/file (where file contains one of the above)");

  if (argc == 2 && string("--help") == argv[1]) {
    usage(argv[0], configurator);
    exit(1);
  }

  Configuration conf;
  try {
    conf = configurator.load(argc, argv);
  } catch (const ConfigurationException& e) {
    cerr << "Configuration error: " << e.what() << endl;
    exit(1);
  }

  // Initialize libprocess.
  process::initialize();

  if (!conf.contains("master")) {
    cerr << "Missing required option --master (-m)" << endl;
    usage(argv[0], configurator);
    exit(1);
  }

  // TODO(vinod): Parse 'master' when we add ZooKeeper support.
  UPID master(conf["master"]);

  if (!master) {
    cerr << "Could not parse --master=" << conf["master"] << endl;
    usage(argv[0], configurator);
    exit(1);
  }

  if (!conf.contains("name")) {
    // TODO(benh): Need to add '--name' as an option.
    cerr << "Missing --name (-n)" << endl;
    usage(argv[0], configurator);
    exit(1);
  }

  LOG(INFO) << "Submitting scheduler ...";

  Protocol<SubmitSchedulerRequest, SubmitSchedulerResponse> submit;

  SubmitSchedulerRequest request;
  request.set_name(conf["name"]);

  Future<SubmitSchedulerResponse> future = submit(master, request);

  future.await(Seconds(5.0));

  if (future.isReady()) {
    if (future.get().okay()) {
      cout << "Scheduler submitted successfully" << endl;
    } else {
      cout << "Failed to submit scheduler" << endl;
    }
  } else {
    cout << "Timed out waiting for response from submitting scheduler" << endl;
  }

  return 0;
}
