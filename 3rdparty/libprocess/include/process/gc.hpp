#ifndef __PROCESS_GC_HPP__
#define __PROCESS_GC_HPP__

#include <map>

#include <process/process.hpp>


namespace process {

class GarbageCollector : public Process<GarbageCollector>
{
public:
  GarbageCollector() : ProcessBase("__gc__") {}
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
  virtual void exited(const UPID& pid)
  {
    if (processes.count(pid) > 0) {
      const ProcessBase* process = processes[pid];
      processes.erase(pid);
      delete process;
    }
  }

private:
  std::map<UPID, const ProcessBase*> processes;
};


extern PID<GarbageCollector> gc;

} // namespace process {

#endif // __PROCESS_GC_HPP__
