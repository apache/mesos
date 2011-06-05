#ifdef __APPLE__
#define _XOPEN_SOURCE
#endif /* __APPLE__ */

#include <sstream>

#include "slave_webui.hpp"
#include "slave_state.hpp"

#ifdef NEXUS_WEBUI

#include <process.hpp>
#include <pthread.h>
#include <Python.h>

extern "C" void init_slave();  // Initializer for the Python slave module

namespace {

PID slave;

}

namespace nexus { namespace internal { namespace slave {


void *runSlaveWebUI(void *)
{
  LOG(INFO) << "Web UI thread started";
  Py_Initialize();
  PyRun_SimpleString("import sys\n"
      "sys.path.append('webui/slave/swig')\n"
      "sys.path.append('webui/common')\n"
      "sys.path.append('third_party/bottle-0.5.6')\n");
  init_slave();
  LOG(INFO) << "Loading webui/slave/webui.py";
  FILE *webui = fopen("webui/slave/webui.py", "r");
  PyRun_SimpleFile(webui, "webui/slave/webui.py");
  fclose(webui);
  Py_Finalize();
}


void startSlaveWebUI(PID slave)
{
  LOG(INFO) << "Starting slave web UI";
  ::slave = slave;
  pthread_t thread;
  pthread_create(&thread, 0, runSlaveWebUI, 0);
}


namespace state {

class StateGetter : public Tuple<Process>
{
public:
  SlaveState *slaveState;

  StateGetter() {}
  ~StateGetter() {}

  void operator () ()
  {
    send(::slave, pack<S2S_GET_STATE>());
    receive();
    int64_t *i = (int64_t *) &slaveState;
    unpack<S2S_GET_STATE_REPLY>(*i);
  }
};


// From slave_state.hpp
SlaveState *get_slave()
{
  StateGetter getter;
  PID pid = Process::spawn(&getter);
  Process::wait(pid);
  return getter.slaveState;
}

} /* namespace state { */

}}} /* namespace nexus { namespace internal { namespace slave { */


#endif /* NEXUS_WEBUI */
