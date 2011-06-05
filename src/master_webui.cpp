#include <sstream>

#include "master_webui.hpp"
#include "master_state.hpp"

#ifdef NEXUS_WEBUI

#include <pthread.h>
#include <Python.h>

extern "C" void init_master();  // Initializer for the Python master module

namespace {

PID master;

}

namespace nexus { namespace internal { namespace master {


void *runMasterWebUI(void* webuiPort)
{
  std::ostringstream oss;
  oss << (const long)webuiPort;
  char webuiPortStr[50]; 
  oss.str().copy(webuiPortStr,20);
  LOG(INFO) << "Web UI thread started";
  Py_Initialize();
  char** nargv = (char**)malloc(sizeof(char*)*2);
  nargv[0] = "webui/master/webui.py";
  nargv[1] = webuiPortStr;
  PySys_SetArgv(2,nargv);
  PyRun_SimpleString("import sys\n"
      "sys.path.append('webui/master/swig')\n"
      "sys.path.append('webui/common')\n"
      "sys.path.append('third_party/bottle-0.5.6')\n");
  init_master();
  LOG(INFO) << "Loading webui/master/webui.py";
  FILE *webui = fopen("webui/master/webui.py", "r");
  PyRun_SimpleFile(webui, "webui/master/webui.py");
  fclose(webui);
  Py_Finalize();
}


void startMasterWebUI(const PID &master, const long &webuiPortNum)
{
  LOG(INFO) << "Starting master web UI";
  ::master = master;
  pthread_t thread;
  pthread_create(&thread, 0, runMasterWebUI, (void*)webuiPortNum);
}


namespace state {

class StateGetter : public Tuple<Process>
{
public:
  MasterState *masterState;

  StateGetter() {}
  ~StateGetter() {}

  void operator () ()
  {
    send(::master, pack<M2M_GET_STATE>());
    receive();
    int64_t *i = (int64_t *) &masterState;
    unpack<M2M_GET_STATE_REPLY>(*i);
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

}}} /* namespace nexus { namespace internal { namespace master { */

#endif /* NEXUS_WEBUI */
