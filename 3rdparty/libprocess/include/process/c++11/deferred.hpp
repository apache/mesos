#ifndef __PROCESS_DEFERRED_HPP__
#define __PROCESS_DEFERRED_HPP__

#include <functional>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/pid.hpp>

#include <stout/preprocessor.hpp>

namespace process {

// Forward declarations (removing these produces cryptic compiler
// errors even though we are just using them to declare friends).
class Executor;
template <typename G> struct _Deferred;


// Acts like a function call but runs within an asynchronous execution
// context such as an Executor or a ProcessBase (enforced because only
// an executor or the 'defer' routines are allowed to create them).
template <typename F>
struct Deferred : std::function<F>
{
private:
  friend class Executor;

  template <typename G> friend struct _Deferred;

  // TODO(benh): Consider removing these in favor of having these
  // functions return _Deferred.
  template <typename T>
  friend Deferred<void(void)>
  defer(const PID<T>& pid, void (T::*method)(void));

  template <typename R, typename T>
  friend Deferred<Future<R>(void)>
  defer(const PID<T>& pid, Future<R> (T::*method)(void));

  template <typename R, typename T>
  friend Deferred<Future<R>(void)>
  defer(const PID<T>& pid, R (T::*method)(void));

  Deferred(const std::function<F>& f) : std::function<F>(f) {}
};


template <typename F>
struct _Deferred
{
  operator Deferred<void()> () const
  {
    if (pid.isNone()) {
      return std::function<void()>(f);
    }

    // We need to explicitly copy the members otherwise we'll
    // implicitly copy 'this' which might not exist at invocation.
    Option<UPID> pid_ = pid;
    F f_ = f;

    return std::function<void()>(
        [=] () {
          dispatch(pid_.get(), std::function<void()>(f_));
        });
  }

  operator std::function<void()> () const
  {
    if (pid.isNone()) {
      return std::function<void()>(f);
    }

    Option<UPID> pid_ = pid;
    F f_ = f;

    return std::function<void()>(
        [=] () {
          dispatch(pid_.get(), std::function<void()>(f_));
        });
  }

  template <typename R>
  operator Deferred<R()> () const
  {
    if (pid.isNone()) {
      return std::function<R()>(f);
    }

    Option<UPID> pid_ = pid;
    F f_ = f;

    return std::function<R()>(
        [=] () {
          return dispatch(pid_.get(), std::function<R()>(f_));
        });
  }

  template <typename R>
  operator std::function<R()> () const
  {
    if (pid.isNone()) {
      return std::function<R()>(f);
    }

    Option<UPID> pid_ = pid;
    F f_ = f;

    return std::function<R()>(
        [=] () {
          return dispatch(pid_.get(), std::function<R()>(f_));
        });
  }

#define TEMPLATE(Z, N, DATA)                                            \
  template <ENUM_PARAMS(N, typename P)>                                 \
  operator Deferred<void(ENUM_PARAMS(N, P))> () const                   \
  {                                                                     \
    if (pid.isNone()) {                                                 \
      return std::function<void(ENUM_PARAMS(N, P))>(f);                 \
    }                                                                   \
                                                                        \
    Option<UPID> pid_ = pid;                                                    \
    F f_ = f;                                                           \
                                                                        \
    return std::function<void(ENUM_PARAMS(N, P))>(                      \
        [=] (ENUM_BINARY_PARAMS(N, P, p)) {                             \
          std::function<void()> f__([=] () {                            \
            f_(ENUM_PARAMS(N, p));                                      \
          });                                                           \
          dispatch(pid_.get(), f__);                                    \
        });                                                             \
  }                                                                     \
                                                                        \
  template <ENUM_PARAMS(N, typename P)>                                 \
  operator std::function<void(ENUM_PARAMS(N, P))> () const              \
  {                                                                     \
    if (pid.isNone()) {                                                 \
      return std::function<void(ENUM_PARAMS(N, P))>(f);                 \
    }                                                                   \
                                                                        \
    Option<UPID> pid_ = pid;                                                    \
    F f_ = f;                                                           \
                                                                        \
    return std::function<void(ENUM_PARAMS(N, P))>(                      \
        [=] (ENUM_BINARY_PARAMS(N, P, p)) {                             \
          std::function<void()> f__([=] () {                            \
            f_(ENUM_PARAMS(N, p));                                      \
          });                                                           \
          dispatch(pid_.get(), f__);                                    \
        });                                                             \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R, ENUM_PARAMS(N, typename P)>                     \
  operator Deferred<R(ENUM_PARAMS(N, P))> () const                      \
  {                                                                     \
    if (pid.isNone()) {                                                 \
      return std::function<R(ENUM_PARAMS(N, P))>(f);                    \
    }                                                                   \
                                                                        \
    Option<UPID> pid_ = pid;                                                    \
    F f_ = f;                                                           \
                                                                        \
    return std::function<R(ENUM_PARAMS(N, P))>(                         \
        [=] (ENUM_BINARY_PARAMS(N, P, p)) {                             \
          std::function<R()> f__([=] () {                               \
            return f_(ENUM_PARAMS(N, p));                               \
          });                                                           \
          return dispatch(pid_.get(), f__);                             \
        });                                                             \
  }                                                                     \
                                                                        \
  template <typename R, ENUM_PARAMS(N, typename P)>                     \
  operator std::function<R(ENUM_PARAMS(N, P))> () const                 \
  {                                                                     \
    if (pid.isNone()) {                                                 \
      return std::function<R(ENUM_PARAMS(N, P))>(f);                    \
    }                                                                   \
                                                                        \
    Option<UPID> pid_ = pid;                                                    \
    F f_ = f;                                                           \
                                                                        \
    return std::function<R(ENUM_PARAMS(N, P))>(                         \
        [=] (ENUM_BINARY_PARAMS(N, P, p)) {                             \
          std::function<R()> f__([=] () {                               \
            return f_(ENUM_PARAMS(N, p));                               \
          });                                                           \
          return dispatch(pid_.get(), f__);                             \
        });                                                             \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

private:
  friend class Executor;

  template <typename G>
  friend _Deferred<G> defer(const UPID& pid, G g);

  template <typename G>
  friend _Deferred<G> defer(G g);

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  friend auto defer(const PID<T>& pid,                                  \
             void (T::*method)(ENUM_PARAMS(N, P)),                      \
             ENUM_BINARY_PARAMS(N, A, a))                               \
    -> _Deferred<decltype(std::bind(std::function<void(ENUM_PARAMS(N, P))>(), ENUM_PARAMS(N, a)))>;

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  friend auto defer(const PID<T>& pid,                                  \
             Future<R> (T::*method)(ENUM_PARAMS(N, P)),                 \
             ENUM_BINARY_PARAMS(N, A, a))                               \
    -> _Deferred<decltype(std::bind(std::function<Future<R>(ENUM_PARAMS(N, P))>(), ENUM_PARAMS(N, a)))>;

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

#define TEMPLATE(Z, N, DATA)                                            \
  template <typename R,                                                 \
            typename T,                                                 \
            ENUM_PARAMS(N, typename P),                                 \
            ENUM_PARAMS(N, typename A)>                                 \
  friend auto defer(const PID<T>& pid,                                  \
             R (T::*method)(ENUM_PARAMS(N, P)),                         \
             ENUM_BINARY_PARAMS(N, A, a))                               \
    -> _Deferred<decltype(std::bind(std::function<Future<R>(ENUM_PARAMS(N, P))>(), ENUM_PARAMS(N, a)))>;

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args A0 -> A9.
#undef TEMPLATE

  _Deferred(UPID pid, F f) : pid(pid), f(f) {}
  _Deferred(F f) : f(f) {}

  Option<UPID> pid;
  F f;
};

} // namespace process {

#endif // __PROCESS_DEFERRED_HPP__
