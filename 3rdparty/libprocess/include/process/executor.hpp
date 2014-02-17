#if __cplusplus >= 201103L
#include <process/c++11/executor.hpp>
#else
#ifndef __PROCESS_EXECUTOR_HPP__
#define __PROCESS_EXECUTOR_HPP__

#include <process/deferred.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/preprocessor.hpp>
#include <stout/thread.hpp>

namespace process {

namespace internal {

// Underlying "process" which handles invoking actual callbacks
// created through an Executor.
class ExecutorProcess : public Process<ExecutorProcess>
{
private:
  friend class process::Executor;

  ExecutorProcess() : ProcessBase(ID::generate("__executor__")) {}
  virtual ~ExecutorProcess() {}

  // Not copyable, not assignable.
  ExecutorProcess(const ExecutorProcess&);
  ExecutorProcess& operator = (const ExecutorProcess&);

  // No arg invoke.
  void invoke(const std::tr1::function<void(void)>& f) { f(); }

  // Args invoke.
#define TEMPLATE(Z, N, DATA)                                            \
  template <ENUM_PARAMS(N, typename A)>                                 \
  void CAT(invoke, N)(                                         \
      const std::tr1::function<void(ENUM_PARAMS(N, A))>& f,    \
      ENUM_BINARY_PARAMS(N, A, a))                             \
  {                                                            \
    f(ENUM_PARAMS(N, a));                                      \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE
};

} // namespace internal {


// Provides an abstraction that can take a standard function object
// and convert it to a 'Deferred'. Each converted function object will
// get invoked serially with respect to one another.
class Executor
{
public:
  Executor()
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
    terminate(process);

    // TODO(benh): Note that this doesn't wait because that could
    // cause a deadlock ... thus, the semantics here are that no more
    // dispatches will occur after this function returns but one may
    // be occuring concurrently.
  }

  // We can't easily use 'std::tr1::_Placeholder<X>' when doing macro
  // expansion via ENUM_BINARY_PARAMS because compilers don't like it
  // when you try and concatenate '<' 'N' '>'. Thus, we typedef them.
private:
#define TEMPLATE(Z, N, DATA)                            \
  typedef std::tr1::_Placeholder<INC(N)> _ ## N;

  REPEAT(10, TEMPLATE, _)
#undef TEMPLATE

public:
  // We provide wrappers for all standard function objects.
  Deferred<void(void)> defer(
      const std::tr1::function<void(void)>& f)
  {
    return Deferred<void(void)>(
        std::tr1::bind(
            &Executor::dispatcher,
            process.self(), f));
  }

#define TEMPLATE(Z, N, DATA)                                            \
  template <ENUM_PARAMS(N, typename A)>                                 \
  Deferred<void(ENUM_PARAMS(N, A))> defer(                              \
      const std::tr1::function<void(ENUM_PARAMS(N, A))>& f)             \
  {                                                                     \
    return Deferred<void(ENUM_PARAMS(N, A))>(                           \
        std::tr1::bind(                                                 \
            &Executor::CAT(dispatcher, N)<ENUM_PARAMS(N, A)>,           \
            process.self(), f,                                          \
            ENUM_BINARY_PARAMS(N, _, () INTERCEPT)));                   \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  // Unfortunately, it is currently difficult to "forward" type
  // information from one result to another, so we must explicilty
  // define wrappers for all std::tr1::bind results. First we start
  // with the non-member std::tr1::bind results.
  Deferred<void(void)> defer(
      const std::tr1::_Bind<void(*(void))(void)>& b)
  {
    return defer(std::tr1::function<void(void)>(b));
  }

#define TEMPLATE(Z, N, DATA)                                            \
  template <ENUM_PARAMS(N, typename A)>                                 \
  Deferred<void(ENUM_PARAMS(N, A))> defer(                              \
      const std::tr1::_Bind<                                            \
      void(*(ENUM_PARAMS(N, _)))                                        \
      (ENUM_PARAMS(N, A))>& b)                                          \
  {                                                                     \
    return defer(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));       \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  // Now the member std::tr1::bind results:
  // 1. Non-const member (function), non-const pointer (receiver).
  // 2. Const member, non-const pointer.
  // 3. Const member, const pointer.
  // 4. Non-const member, non-const reference.
  // 5. Const member, non-const reference.
  // 6. Const member, const reference.
  // 7. Non-const member, value.
  // 8. Const member, value.
#define TEMPLATE(Z, N, DATA)                                            \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  Deferred<void(ENUM_PARAMS(N, A))> defer(                              \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A))>                                    \
      (T* ENUM_TRAILING_PARAMS(N, _))>& b)                              \
  {                                                                     \
    return defer(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));       \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  Deferred<void(ENUM_PARAMS(N, A))> defer(                              \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A)) const>                              \
      (T* ENUM_TRAILING_PARAMS(N, _))>& b)                              \
  {                                                                     \
    return defer(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));       \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  Deferred<void(ENUM_PARAMS(N, A))> defer(                              \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A)) const>                              \
      (const T* ENUM_TRAILING_PARAMS(N, _))>& b)                        \
  {                                                                     \
    return defer(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));       \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  Deferred<void(ENUM_PARAMS(N, A))> defer(                              \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A))>                                    \
      (std::tr1::reference_wrapper<T> ENUM_TRAILING_PARAMS(N, _))>& b)  \
  {                                                                     \
    return defer(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));       \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  Deferred<void(ENUM_PARAMS(N, A))> defer(                              \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A)) const>                              \
      (std::tr1::reference_wrapper<T> ENUM_TRAILING_PARAMS(N, _))>& b)  \
  {                                                                     \
    return defer(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));       \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  Deferred<void(ENUM_PARAMS(N, A))> defer(                              \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A)) const>                              \
      (std::tr1::reference_wrapper<const T> ENUM_TRAILING_PARAMS(N, _))>& b) \
  {                                                                     \
    return defer(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));       \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  Deferred<void(ENUM_PARAMS(N, A))> defer(                              \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A))>                                    \
      (T ENUM_TRAILING_PARAMS(N, _))>& b)                               \
  {                                                                     \
    return defer(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));       \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  Deferred<void(ENUM_PARAMS(N, A))> defer(                              \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A)) const>                              \
      (T ENUM_TRAILING_PARAMS(N, _))>& b)                               \
  {                                                                     \
    return defer(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));       \
  }

  REPEAT(11, TEMPLATE, _) // No args and args A0 -> A9.
#undef TEMPLATE

private:
  // Not copyable, not assignable.
  Executor(const Executor&);
  Executor& operator = (const Executor&);

  static void dispatcher(
      const PID<internal::ExecutorProcess>& pid,
      const std::tr1::function<void(void)>& f)
  {
    // TODO(benh): Why not just use internal::dispatch?
    dispatch(pid, &internal::ExecutorProcess::invoke, f);
  }

#define TEMPLATE(Z, N, DATA)                                            \
  template <ENUM_PARAMS(N, typename A)>                                 \
  static void CAT(dispatcher, N)(                                       \
      const PID<internal::ExecutorProcess>& pid,                        \
      const std::tr1::function<void(ENUM_PARAMS(N, A))>& f,             \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    dispatch(                                                           \
        pid,                                                            \
        &internal::ExecutorProcess::CAT(invoke, N)<ENUM_PARAMS(N, A)>,  \
        f, ENUM_PARAMS(N, a));                                          \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  internal::ExecutorProcess process;
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
#endif // __cplusplus >= 201103L
