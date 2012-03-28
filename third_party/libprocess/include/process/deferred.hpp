#ifndef __PROCESS_DEFERRED_HPP__
#define __PROCESS_DEFERRED_HPP__

#include <tr1/functional>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/preprocessor.hpp>

namespace process {

// Acts like a function call but runs within an asynchronous execution
// context such as an Executor or a ProcessBase (since only an
// executor or the 'defer' routines are allowed to create them).
template <typename F>
struct deferred : std::tr1::function<F>
{
private:
  // Only an Executor and the 'defer' routines can create these.
  friend class Executor;

  template <typename _F>
  friend struct _Defer;

  deferred(const std::tr1::function<F>& f) : std::tr1::function<F>(f) {}
};

} // namespace process {

#endif // __PROCESS_DEFERRED_HPP__
