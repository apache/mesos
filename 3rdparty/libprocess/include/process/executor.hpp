#ifndef __PROCESS_EXECUTOR_HPP__
#define __PROCESS_EXECUTOR_HPP__

#include <process/defer.hpp>
#include <process/deferred.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/thread.hpp>

namespace process {

// Provides an abstraction that can take a standard function object
// and defer it without needing a process. Each converted function
// object will get execute serially with respect to one another when
// invoked.
class Executor
{
public:
  Executor() : process(ID::generate("__executor__"))
  {
    spawn(process);
  }

  ~Executor()
  {
    terminate(process);
    wait(process);
  }

  void stop()
  {
    terminate(&process);

    // TODO(benh): Note that this doesn't wait because that could
    // cause a deadlock ... thus, the semantics here are that no more
    // dispatches will occur after this function returns but one may
    // be occuring concurrently.
  }

  template <typename F>
  _Deferred<F> defer(F&& f)
  {
    return _Deferred<F>(process.self(), std::forward<F>(f));
  }


private:
  // Not copyable, not assignable.
  Executor(const Executor&);
  Executor& operator = (const Executor&);

  ProcessBase process;
};


// Per thread executor pointer. The extra level of indirection from
// _executor_ to __executor__ is used in order to take advantage of
// the ThreadLocal operators without needing the extra dereference as
// well as lazily construct the actual executor.
extern ThreadLocal<Executor>* _executor_;

#define __executor__                                                    \
  (*_executor_ == NULL ? *_executor_ = new Executor() : *_executor_)

} // namespace process {

#endif // __PROCESS_EXECUTOR_HPP__
