#ifndef __PROCESS_ASYNC_HPP__
#define __PROCESS_ASYNC_HPP__

#include <process/dispatch.hpp>

#include <boost/preprocessor/cat.hpp>

#include <boost/preprocessor/arithmetic/inc.hpp>

#include <boost/preprocessor/facilities/intercept.hpp>

#include <boost/preprocessor/repetition/enum_params.hpp>
#include <boost/preprocessor/repetition/enum_binary_params.hpp>
#include <boost/preprocessor/repetition/enum_trailing_params.hpp>
#include <boost/preprocessor/repetition/repeat.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>

#define CAT BOOST_PP_CAT
#define INC BOOST_PP_INC
#define INTERCEPT BOOST_PP_INTERCEPT
#define ENUM_PARAMS BOOST_PP_ENUM_PARAMS
#define ENUM_BINARY_PARAMS BOOST_PP_ENUM_BINARY_PARAMS
#define ENUM_TRAILING_PARAMS BOOST_PP_ENUM_TRAILING_PARAMS
#define REPEAT BOOST_PP_REPEAT
#define REPEAT_FROM_TO BOOST_PP_REPEAT_FROM_TO


namespace async {

// Acts like a function call but performs an asynchronous
// dispatch. Thus, the "caller" knows that it will not block, for
// example, within the "callee" callback.
template <typename F>
struct dispatch : std::tr1::function<F>
{
private:
  friend class Dispatch; // Only class capable of creating these.
  dispatch(const std::tr1::function<F>& f) : std::tr1::function<F>(f) {}
};


// Underlying "process" which handles invoking actual callbacks
// created through a Dispatch object.
class DispatchProcess : public process::Process<DispatchProcess>
{
private:
  friend class Dispatch;

  DispatchProcess() {}
  ~DispatchProcess() {}

  // Not copyable, not assignable.
  DispatchProcess(const DispatchProcess&);
  DispatchProcess& operator = (const DispatchProcess&);

  // No arg invoke.
  void invoke(const std::tr1::function<void()>& f) { f(); }

  // Args invoke.
#define APPLY(Z, N, DATA)                                      \
  template <ENUM_PARAMS(N, typename A)>                        \
  void CAT(invoke, N)(                                         \
      const std::tr1::function<void(ENUM_PARAMS(N, A))>& f,    \
      ENUM_BINARY_PARAMS(N, A, a))                             \
  {                                                            \
    f(ENUM_PARAMS(N, a));                                      \
  }

  REPEAT_FROM_TO(1, 11, APPLY, _) // Args A0 -> A9.
#undef APPLY
};


// Provides an abstraction that can take a standard function object
// and convert it to a dispatch object. Each converted function object
// will get invoked serially with respect to one another.
class Dispatch
{
public:
  Dispatch() {
    process::spawn(process);
  }

  ~Dispatch()
  {
    process::terminate(process);
    process::wait(process);
  }

  // We can't easily use 'std::tr1::_Placeholder<X>' when doing macro
  // expansion via ENUM_BINARY_PARAMS because compilers don't like it
  // when you try and concatenate '<' 'N' '>'. Thus, we typedef them.
private:
#define APPLY(Z, N, DATA)                               \
  typedef std::tr1::_Placeholder<INC(N)> _ ## N;

  REPEAT(10, APPLY, _)
#undef APPLY

public:
  // We provide wrappers for all standard function objects.
  dispatch<void()> operator () (
      const std::tr1::function<void()>& f)
  {
    return dispatch<void()>(
        std::tr1::bind(
            &Dispatch::dispatcher,
            process.self(), f));
  }

#define APPLY(Z, N, DATA)                                               \
  template <ENUM_PARAMS(N, typename A)>                                 \
  dispatch<void(ENUM_PARAMS(N, A))> operator () (                       \
      const std::tr1::function<void(ENUM_PARAMS(N, A))>& f)             \
  {                                                                     \
    return dispatch<void(ENUM_PARAMS(N, A))>(                           \
        std::tr1::bind(                                                 \
            &Dispatch::CAT(dispatcher, N)<ENUM_PARAMS(N, A)>,           \
            process.self(), f,                                          \
            ENUM_BINARY_PARAMS(N, _, () INTERCEPT)));                   \
  }

  REPEAT_FROM_TO(1, 11, APPLY, _) // Args A0 -> A9.
#undef APPLY

  // Unfortunately, it is currently difficult to "forward" type
  // information from one result to another, so we must explicilty
  // define wrappers for all std::tr1::bind results. First we start
  // with the non-member std::tr1::bind results.
  dispatch<void()> operator () (
      const std::tr1::_Bind<void(*())()>& b)
  {
    return (*this)(std::tr1::function<void()>(b));
  }

#define APPLY(Z, N, DATA)                                               \
  template <ENUM_PARAMS(N, typename A)>                                 \
  dispatch<void(ENUM_PARAMS(N, A))> operator () (                       \
      const std::tr1::_Bind<                                            \
      void(*(ENUM_PARAMS(N, _)))                                        \
      (ENUM_PARAMS(N, A))>& b)                                          \
  {                                                                     \
    return (*this)(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));     \
  }

  REPEAT_FROM_TO(1, 11, APPLY, _) // Args A0 -> A9.
#undef APPLY

  // Now the member std::tr1::bind results:
  // 1. Non-const member (function), non-const pointer (receiver).
  // 2. Const member, non-const pointer.
  // 3. Const member, const pointer.
  // 4. Non-const member, non-const reference.
  // 5. Const member, non-const reference.
  // 6. Const member, const reference.
  // 7. Non-const member, value.
  // 8. Const member, value.
#define APPLY(Z, N, DATA)                                               \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  dispatch<void(ENUM_PARAMS(N, A))> operator () (                       \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A))>                                    \
      (T* ENUM_TRAILING_PARAMS(N, _))>& b)                              \
  {                                                                     \
    return (*this)(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));     \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  dispatch<void(ENUM_PARAMS(N, A))> operator () (                       \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A)) const>                              \
      (T* ENUM_TRAILING_PARAMS(N, _))>& b)                              \
  {                                                                     \
    return (*this)(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));     \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  dispatch<void(ENUM_PARAMS(N, A))> operator () (                       \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A)) const>                              \
      (const T* ENUM_TRAILING_PARAMS(N, _))>& b)                        \
  {                                                                     \
    return (*this)(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));     \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  dispatch<void(ENUM_PARAMS(N, A))> operator () (                       \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A))>                                    \
      (std::tr1::reference_wrapper<T> ENUM_TRAILING_PARAMS(N, _))>& b)  \
  {                                                                     \
    return (*this)(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));     \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  dispatch<void(ENUM_PARAMS(N, A))> operator () (                       \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A)) const>                              \
      (std::tr1::reference_wrapper<T> ENUM_TRAILING_PARAMS(N, _))>& b)  \
  {                                                                     \
    return (*this)(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));     \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  dispatch<void(ENUM_PARAMS(N, A))> operator () (                       \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A)) const>                              \
      (std::tr1::reference_wrapper<const T> ENUM_TRAILING_PARAMS(N, _))>& b) \
  {                                                                     \
    return (*this)(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));     \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  dispatch<void(ENUM_PARAMS(N, A))> operator () (                       \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A))>                                    \
      (T ENUM_TRAILING_PARAMS(N, _))>& b)                               \
  {                                                                     \
    return (*this)(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));     \
  }                                                                     \
                                                                        \
  template <typename T ENUM_TRAILING_PARAMS(N, typename A)>             \
  dispatch<void(ENUM_PARAMS(N, A))> operator () (                       \
      const std::tr1::_Bind<std::tr1::_Mem_fn<                          \
      void(T::*)(ENUM_PARAMS(N, A)) const>                              \
      (T ENUM_TRAILING_PARAMS(N, _))>& b)                               \
  {                                                                     \
    return (*this)(std::tr1::function<void(ENUM_PARAMS(N, A))>(b));     \
  }

  REPEAT(11, APPLY, _) // No args and args A0 -> A9.
#undef APPLY

private:
  // Not copyable, not assignable.
  Dispatch(const Dispatch&);
  Dispatch& operator = (const Dispatch&);

  static void dispatcher(
      const process::PID<DispatchProcess>& pid,
      const std::tr1::function<void()>& f)
  {
    process::dispatch(pid, &DispatchProcess::invoke, f);
  }

#define APPLY(Z, N, DATA)                                               \
  template <ENUM_PARAMS(N, typename A)>                                 \
  static void CAT(dispatcher, N)(                                       \
      const process::PID<DispatchProcess>& pid,                         \
      const std::tr1::function<void(ENUM_PARAMS(N, A))>& f,             \
      ENUM_BINARY_PARAMS(N, A, a))                                      \
  {                                                                     \
    process::dispatch(                                                  \
        pid,                                                            \
        &DispatchProcess::CAT(invoke, N)<ENUM_PARAMS(N, A)>,            \
        f, ENUM_PARAMS(N, a));                                          \
  }

  REPEAT_FROM_TO(1, 11, APPLY, _) // Args A0 -> A9.
#undef APPLY

  DispatchProcess process;
};

} // namespace async {

#undef CAT
#undef INC
#undef INTERCEPT
#undef ENUM_PARAMS
#undef ENUM_BINARY_PARAMS
#undef ENUM_TRAILING_PARAMS
#undef REPEAT
#undef REPEAT_FROM_TO

#endif // __PROCESS_ASYNC_HPP__
