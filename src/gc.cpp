

namespace process { namespace record {

enum { SHUTDOWN = MESOS_MESSAGES, CLEANUP };

RECORD(SHUTDOWN, ());
RECORD(CLEANUP, (SchedulerProcess *));

}}

static PID gc = { 0 };


send(gc, pack<CLEANUP>(____ptr____(this)));



/**
 * GarbageCollector process, responsible for cleaning up instances of
 * SchedulerProcess in a thread-safe way.
 */

class GarbageCollector : public RecordProcess
{
protected:
  void operator () ()
  {
    while(true) {
      switch(receive()) {
        case CLEANUP: {
//       SchedulerProcess *process;
//           unpack<CLEANUP>(process);
//           wait(process);
//       delete process;
          break;
        }
      }
    }
  }
};



// TODO(benh): Do initialization properly for non-gcc compilers.

static void __attribute__ ((constructor)) init()
{
  gc = Process::spawn(new GarbageCollector());
}

