#include <sstream>

#include "slave_webui.hpp"
#include "slave_state.hpp"

#ifdef MESOS_WEBUI

#include <process.hpp>
#include <pthread.h>
#include <Python.h>

extern "C" void init_slave();  // Initializer for the Python slave module

namespace {

PID slave;

}

namespace mesos { namespace internal { namespace slave {


void *runSlaveWebUI(void* webuiPort)
{
  LOG(INFO) << "Web UI thread started";
  Py_Initialize();
  char* nargv[2]; 
  nargv[0] = const_cast<char*>("webui/master/webui.py");
  nargv[1] = reinterpret_cast<char*>(webuiPort);
  PySys_SetArgv(2,nargv);
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


void startSlaveWebUI(const PID &slave, char* webuiPort)
{
  LOG(INFO) << "Starting slave web UI";
  ::slave = slave;
  pthread_t thread;
  pthread_create(&thread, 0, runSlaveWebUI, webuiPort);
}


namespace state {

class StateGetter : public MesosProcess
{
public:
  SlaveState *slaveState;

  StateGetter() {}
  ~StateGetter() {}

  void operator () ()
  {
    send(::slave, pack<S2S_GET_STATE>());
    receive();
    CHECK(msgid() == S2S_GET_STATE_REPLY);
    slaveState = unpack<S2S_GET_STATE_REPLY, 0>(body());
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

}}} /* namespace mesos { namespace internal { namespace slave { */


#endif /* MESOS_WEBUI */
