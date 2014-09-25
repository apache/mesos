#ifndef PROCESS_REFERENCE_HPP
#define PROCESS_REFERENCE_HPP

#include <process/process.hpp>

namespace process {

// Provides reference counting semantics for a process pointer.
class ProcessReference
{
public:
  ProcessReference() : process(NULL) {}

  ~ProcessReference()
  {
    cleanup();
  }

  ProcessReference(const ProcessReference& that)
  {
    copy(that);
  }

  ProcessReference& operator = (const ProcessReference& that)
  {
    if (this != &that) {
      cleanup();
      copy(that);
    }
    return *this;
  }

  ProcessBase* operator -> ()
  {
    return process;
  }

  operator ProcessBase* ()
  {
    return process;
  }

  operator bool () const
  {
    return process != NULL;
  }

private:
  friend class ProcessManager; // For ProcessManager::use.

  explicit ProcessReference(ProcessBase* _process)
    : process(_process)
  {
    if (process != NULL) {
      __sync_fetch_and_add(&(process->refs), 1);
    }
  }

  void copy(const ProcessReference& that)
  {
    process = that.process;

    if (process != NULL) {
      // There should be at least one reference to the process, so
      // we don't need to worry about checking if it's exiting or
      // not, since we know we can always create another reference.
      CHECK(process->refs > 0);
      __sync_fetch_and_add(&(process->refs), 1);
    }
  }

  void cleanup()
  {
    if (process != NULL) {
      __sync_fetch_and_sub(&(process->refs), 1);
    }
  }

  ProcessBase* process;
};

} // namespace process {

#endif // PROCESS_REFERENCE_HPP
