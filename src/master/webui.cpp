#include <pthread.h>

#include <sstream>
#include <string>

#include "webui.hpp"
#include "state.hpp"

#include "configurator/configuration.hpp"


#ifdef MESOS_WEBUI

#include <Python.h>

using std::string;


extern "C" void init_master();  // Initializer for the Python master module

namespace {

PID master;
string webuiPort;
string logDir;

}

namespace mesos { namespace internal { namespace master {


void *runMasterWebUI(void *)
{
  LOG(INFO) << "Web UI thread started";
  Py_Initialize();
  char* argv[3];
  argv[0] = const_cast<char*>("webui/master/webui.py");
  argv[1] = const_cast<char*>(webuiPort.c_str());
  argv[2] = const_cast<char*>(logDir.c_str());
  PySys_SetArgv(3, argv);
  PyRun_SimpleString("import sys\n"
      "sys.path.append('webui/master/swig')\n"
      "sys.path.append('webui/common')\n"
      "sys.path.append('webui/bottle-0.8.3')\n");
  init_master();
  LOG(INFO) << "Loading webui/master/webui.py";
  FILE *webui = fopen("webui/master/webui.py", "r");
  PyRun_SimpleFile(webui, "webui/master/webui.py");
  fclose(webui);
  Py_Finalize();
}


void startMasterWebUI(const PID &master, const Configuration &conf)
{
  // TODO(*): It would be nice if we didn't have to be specifying
  // default values for configuration options in the code like
  // this. For example, we specify /tmp for log_dir because that is
  // what glog does, but it would be nice if at this point in the game
  // all of the configuration options have been set (from defaults or
  // from the command line, environment, or configuration file) and we
  // can just query what their values are.
  webuiPort = conf.get("webui_port", "8080");
  logDir = conf.get("log_dir", FLAGS_log_dir);

  LOG(INFO) << "Starting master web UI on port " << webuiPort;

  ::master = master;
  pthread_t thread;
  pthread_create(&thread, 0, runMasterWebUI, NULL);
}


namespace state {

class StateGetter : public MesosProcess
{
public:
  MasterState *masterState;

  StateGetter() {}
  ~StateGetter() {}

  virtual void operator () ()
  {
    send(::master, M2M_GET_STATE);
    receive();
    CHECK(msgid() == M2M_GET_STATE_REPLY);

    const MSG<M2M_GET_STATE_REPLY>& msg = message();

    masterState =
      *(state::MasterState **) msg.pointer().data();
  }
};


// From master_state.hpp
MasterState *get_master()
{
  StateGetter getter;
  PID pid = Process::spawn(&getter);
  Process::wait(pid);
  return getter.masterState;
}

} /* namespace state { */

}}} /* namespace mesos { namespace internal { namespace master { */

#endif /* MESOS_WEBUI */
