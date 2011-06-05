#ifndef __GC_HPP__
#define __GC_HPP__

#include <map>

#include <process/process.hpp>


namespace process {

class GarbageCollector : public Process<GarbageCollector>
{
public:
  GarbageCollector() {}
  virtual ~GarbageCollector() {}

  template <typename T>
  void manage(const T* t)
  {
    const ProcessBase* process = t;
    if (process != NULL) {
      processes[process->self()] = process;
      link(process->self());
    }
  }

protected:
  virtual void operator () ()
  {
    while (true) {
      serve();
      if (name() == EXITED && processes.count(from()) > 0) {
        const ProcessBase* process = processes[from()];
        processes.erase(from());
        delete process;
      }
    }
  }

private:
  std::map<UPID, const ProcessBase*> processes;
};


extern PID<GarbageCollector> gc;

} // namespace process {

#endif // __GC_HPP__
