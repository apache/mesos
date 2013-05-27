#ifndef __PROCESS_DISPATCH_HPP__
#define __PROCESS_DISPATCH_HPP__

#include <string>

#include <tr1/functional>
#include <tr1/memory> // TODO(benh): Replace all shared_ptr with unique_ptr.

#include <process/process.hpp>

#include <stout/preprocessor.hpp>

namespace process {

// The dispatch mechanism enables you to "schedule" a method to get
// invoked on a process. The result of that method invocation is
// accessible via the future that is returned by the dispatch method
// (note, however, that it might not be the _same_ future as the one
// returned from the method, if the method even returns a future, see
// below). Assuming some class 'Fibonacci' has a (visible) method
// named 'compute' that takes an integer, N (and returns the Nth
// fibonacci number) you might use dispatch like so:
//
// PID<Fibonacci> pid = spawn(new Fibonacci(), true); // Use the GC.
// Future<int> f = dispatch(pid, &Fibonacci::compute, 10);
//
// Because the pid argument is "typed" we can ensure that methods are
// only invoked on processes that are actually of that type. Providing
// this mechanism for varying numbers of function types and arguments
// requires support for variadic templates, slated to be released in
// C++11. Until then, we use the Boost preprocessor macros to
// accomplish the same thing (all be it less cleanly). See below for
// those definitions.
//
// Dispatching is done via a level of indirection. The dispatch
// routine itself creates a promise that is passed as an argument to a
// partially applied 'dispatcher' function (defined below). The
// dispatcher routines get passed to the actual process via an
// internal routine called, not suprisingly, 'dispatch', defined
// below:

namespace internal {

// The internal dispatch routine schedules a function to get invoked
// within the context of the process associated with the specified pid
// (first argument), unless that process is no longer valid. Note that
// this routine does not expect anything in particular about the
// specified function (second argument). The semantics are simple: the
// function gets applied/invoked with the process as its first
// argument. Currently we wrap the function in a shared_ptr but this
// will probably change in the future to unique_ptr (or a variant).
void dispatch(
    const UPID& pid,
    const std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> >& f,
    const std::string& method = std::string());

// For each return type (void, future, value) there is a dispatcher
// function which should complete the picture. Given the process
// argument these routines downcast the process to the correct subtype
// and invoke the thunk using the subtype as the argument
// (receiver). Note that we must use dynamic_cast because we permit a
// process to use multiple inheritance (e.g., to expose multiple
// callback interfaces).

template <typename T>
void vdispatcher(
    ProcessBase* process,
    std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk)
{
  assert(process != NULL);
  T* t = dynamic_cast<T*>(process);
  assert(t != NULL);
  (*thunk)(t);
}


template <typename R, typename T>
void pdispatcher(
    ProcessBase* process,
    std::tr1::shared_ptr<std::tr1::function<Future<R>(T*)> > thunk,
    std::tr1::shared_ptr<Promise<R> > promise)
{
  assert(process != NULL);
  T* t = dynamic_cast<T*>(process);
  assert(t != NULL);
  promise->associate((*thunk)(t));
}


template <typename R, typename T>
void rdispatcher(
    ProcessBase* process,
    std::tr1::shared_ptr<std::tr1::function<R(T*)> > thunk,
    std::tr1::shared_ptr<Promise<R> > promise)
{
  assert(process != NULL);
  T* t = dynamic_cast<T*>(process);
  assert(t != NULL);
  promise->set((*thunk)(t));
}


// Canonicalizes a pointer to a member function (i.e., method) into a
// bytes representation for comparison (e.g., in tests).
template <typename Method>
std::string canonicalize(Method method)
{
  return std::string(reinterpret_cast<const char*>(&method), sizeof(method));
}

} // namespace internal {


// Okay, now for the definition of the dispatch routines
// themselves. For each routine we provide the version in C++11 using
// variadic templates so the reader can see what the Boost
// preprocessor macros are effectively providing. Using C++11 closures
// would shorten these definitions even more.
//
// First, definitions of dispatch for methods returning void:
//
// template <typename T, typename ...P>
// void dispatch(
//     const PID<T>& pid,
//     void (T::*method)(P...),
//     P... p)
// {
//   std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(
//       new std::tr1::function<void(T*)>(
//           std::tr1::bind(method,
//                          std::tr1::placeholders::_1,
//                          std::forward<P>(p)...)));
//
//   std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher(
//       new std::tr1::function<void(ProcessBase*)>(
//           std::tr1::bind(&internal::vdispatcher<T>,
//                          std::tr1::placeholders::_1,
//                          thunk)));
//
//   internal::dispatch(pid, dispatcher, internal::canonicalize(method));
// }

template <typename T>
void dispatch(
    const PID<T>& pid,
    void (T::*method)(void))
{
  std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(
      new std::tr1::function<void(T*)>(
          std::tr1::bind(method, std::tr1::placeholders::_1)));

  std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher(
      new std::tr1::function<void(ProcessBase*)>(
          std::tr1::bind(&internal::vdispatcher<T>,
                         std::tr1::placeholders::_1,
                         thunk)));

  internal::dispatch(pid, dispatcher, internal::canonicalize(method));
}

template <typename T>
void dispatch(
    const Process<T>& process,
    void (T::*method)(void))
{
  dispatch(process.self(), method);
}

template <typename T>
void dispatch(
    const Process<T>* process,
    void (T::*method)(void))
{
  dispatch(process->self(), method);
}

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  void dispatch(                                                        \
      const PID<T>& pid,                                                \
      void (T::*method)(ENUM_PARAMS(N, P)),                             \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    std::tr1::shared_ptr<std::tr1::function<void(T*)> > thunk(          \
        new std::tr1::function<void(T*)>(                               \
            std::tr1::bind(method,                                      \
                           std::tr1::placeholders::_1,                  \
                           ENUM_PARAMS(N, a))));                        \
                                                                        \
    std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher( \
        new std::tr1::function<void(ProcessBase*)>(                     \
            std::tr1::bind(&internal::vdispatcher<T>,                   \
                           std::tr1::placeholders::_1,                  \
                           thunk)));                                    \
                                                                        \
    internal::dispatch(pid, dispatcher, internal::canonicalize(method)); \
  }                                                                     \
                                                                        \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  void dispatch(                                                        \
      const Process<T>& process,                                        \
      void (T::*method)(ENUM_PARAMS(N, P)),                             \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    dispatch(process.self(), method, ENUM_PARAMS(N, a));                \
  }                                                                     \
                                                                        \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  void dispatch(                                                        \
      const Process<T>* process,                                        \
      void (T::*method)(ENUM_PARAMS(N, P)),                             \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    dispatch(process->self(), method, ENUM_PARAMS(N, a));               \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE


// Next, definitions of methods returning a future:
//
// template <typename R, typename T, typename ...P>
// Future<R> dispatch(
//     const PID<T>& pid,
//     Future<R> (T::*method)(P...),
//     P... p)
// {
//   std::tr1::shared_ptr<std::tr1::function<Future<R>(T*)> > thunk(
//       new std::tr1::function<Future<R>(T*)>(
//           std::tr1::bind(method,
//                          std::tr1::placeholders::_1,
//                          std::forward<P>(p)...)));
//
//   std::tr1::shared_ptr<Promise<R> > promise(new Promise<R>());
//   Future<R> future = promise->future();
//
//   std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher(
//       new std::tr1::function<void(ProcessBase*)>(
//           std::tr1::bind(&internal::pdispatcher<R, T>,
//                          std::tr1::placeholders::_1,
//                          thunk, promise)));
//
//   internal::dispatch(pid, dispatcher, internal::canonicalize(method));
//
//   return future;
// }

template <typename R, typename T>
Future<R> dispatch(
    const PID<T>& pid,
    Future<R> (T::*method)(void))
{
  std::tr1::shared_ptr<std::tr1::function<Future<R>(T*)> > thunk(
      new std::tr1::function<Future<R>(T*)>(
          std::tr1::bind(method, std::tr1::placeholders::_1)));

  std::tr1::shared_ptr<Promise<R> > promise(new Promise<R>());
  Future<R> future = promise->future();

  std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher(
      new std::tr1::function<void(ProcessBase*)>(
          std::tr1::bind(&internal::pdispatcher<R, T>,
                         std::tr1::placeholders::_1,
                         thunk, promise)));

  internal::dispatch(pid, dispatcher, internal::canonicalize(method));

  return future;
}

template <typename R, typename T>
Future<R> dispatch(
    const Process<T>& process,
    Future<R> (T::*method)(void))
{
  return dispatch(process.self(), method);
}

template <typename R, typename T>
Future<R> dispatch(
    const Process<T>* process,
    Future<R> (T::*method)(void))
{
  return dispatch(process->self(), method);
}

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> dispatch(                                                   \
      const PID<T>& pid,                                                \
      Future<R> (T::*method)(ENUM_PARAMS(N, P)),                        \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    std::tr1::shared_ptr<std::tr1::function<Future<R>(T*)> > thunk(     \
        new std::tr1::function<Future<R>(T*)>(                          \
            std::tr1::bind(method,                                      \
                           std::tr1::placeholders::_1,                  \
                           ENUM_PARAMS(N, a))));                        \
                                                                        \
    std::tr1::shared_ptr<Promise<R> > promise(new Promise<R>());        \
    Future<R> future = promise->future();                               \
                                                                        \
    std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher( \
        new std::tr1::function<void(ProcessBase*)>(                     \
            std::tr1::bind(&internal::pdispatcher<R, T>,                \
                           std::tr1::placeholders::_1,                  \
                           thunk, promise)));                           \
                                                                        \
    internal::dispatch(pid, dispatcher, internal::canonicalize(method)); \
                                                                        \
    return future;                                                      \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> dispatch(                                                   \
      const Process<T>& process,                                        \
      Future<R> (T::*method)(ENUM_PARAMS(N, P)),                        \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    return dispatch(process.self(), method, ENUM_PARAMS(N, a));         \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> dispatch(                                                   \
      const Process<T>* process,                                        \
      Future<R> (T::*method)(ENUM_PARAMS(N, P)),                        \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    return dispatch(process->self(), method, ENUM_PARAMS(N, a));        \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE


// Next, definitions of methods returning a value.
//
// template <typename R, typename T, typename ...P>
// Future<R> dispatch(
//     const PID<T>& pid,
//     R (T::*method)(P...),
//     P... p)
// {
//   std::tr1::shared_ptr<std::tr1::function<R(T*)> > thunk(
//       new std::tr1::function<R(T*)>(
//           std::tr1::bind(method,
//                          std::tr1::placeholders::_1,
//                          std::forward<P>(p)...)));
//
//   std::tr1::shared_ptr<Promise<R> > promise(new Promise<R>());
//   Future<R> future = promise->future();
//
//   std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher(
//       new std::tr1::function<void(ProcessBase*)>(
//           std::tr1::bind(&internal::rdispatcher<R, T>,
//                          std::tr1::placeholders::_1,
//                          thunk, promise)));
//
//   internal::dispatch(pid, dispatcher, internal::canonicalize(method));
//
//   return future;
// }

template <typename R, typename T>
Future<R> dispatch(
    const PID<T>& pid,
    R (T::*method)(void))
{
  std::tr1::shared_ptr<std::tr1::function<R(T*)> > thunk(
      new std::tr1::function<R(T*)>(
          std::tr1::bind(method, std::tr1::placeholders::_1)));

  std::tr1::shared_ptr<Promise<R> > promise(new Promise<R>());
  Future<R> future = promise->future();

  std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher(
      new std::tr1::function<void(ProcessBase*)>(
          std::tr1::bind(&internal::rdispatcher<R, T>,
                         std::tr1::placeholders::_1,
                         thunk, promise)));

  internal::dispatch(pid, dispatcher, internal::canonicalize(method));

  return future;
}

template <typename R, typename T>
Future<R> dispatch(
    const Process<T>& process,
    R (T::*method)(void))
{
  return dispatch(process.self(), method);
}

template <typename R, typename T>
Future<R> dispatch(
    const Process<T>* process,
    R (T::*method)(void))
{
  return dispatch(process->self(), method);
}

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> dispatch(                                                   \
      const PID<T>& pid,                                                \
      R (T::*method)(ENUM_PARAMS(N, P)),                                \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    std::tr1::shared_ptr<std::tr1::function<R(T*)> > thunk(             \
        new std::tr1::function<R(T*)>(                                  \
            std::tr1::bind(method,                                      \
                           std::tr1::placeholders::_1,                  \
                           ENUM_PARAMS(N, a))));                        \
                                                                        \
    std::tr1::shared_ptr<Promise<R> > promise(new Promise<R>());        \
    Future<R> future = promise->future();                               \
                                                                        \
    std::tr1::shared_ptr<std::tr1::function<void(ProcessBase*)> > dispatcher( \
        new std::tr1::function<void(ProcessBase*)>(                     \
            std::tr1::bind(&internal::rdispatcher<R, T>,                \
                           std::tr1::placeholders::_1,                  \
                           thunk, promise)));                           \
                                                                        \
    internal::dispatch(pid, dispatcher, internal::canonicalize(method)); \
                                                                        \
    return future;                                                      \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> dispatch(                                                   \
      const Process<T>& process,                                        \
      R (T::*method)(ENUM_PARAMS(N, P)),                                \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    return dispatch(process.self(), method, ENUM_PARAMS(N, a));         \
  }                                                                     \
                                                                        \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  Future<R> dispatch(                                                   \
      const Process<T>* process,                                        \
      R (T::*method)(ENUM_PARAMS(N, P)),                                \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    return dispatch(process->self(), method, ENUM_PARAMS(N, a));        \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

} // namespace process {

#endif // __PROCESS_DISPATCH_HPP__
