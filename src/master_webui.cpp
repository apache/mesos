#ifdef __APPLE__
#define _XOPEN_SOURCE
#endif /* __APPLE__ */

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


void *runMasterWebUI(void *)
{
  LOG(INFO) << "Web UI thread started";
  Py_Initialize();
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


void startMasterWebUI(PID master)
{
  LOG(INFO) << "Starting master web UI";
  ::master = master;
  pthread_t thread;
  pthread_create(&thread, 0, runMasterWebUI, 0);
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
