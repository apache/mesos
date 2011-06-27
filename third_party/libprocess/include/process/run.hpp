#ifndef __PROCESS_RUN_HPP__
#define __PROCESS_RUN_HPP__

#include <process/process.hpp>


namespace process {

template <typename R>
Future<R> run(R (*method)());


template <typename R, typename P1, typename A1>
Future<R> run(R (*method)(P1), A1 a1);


template <typename R,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> run(R (*method)(P1, P2), A1 a1, A2 a2);


template <typename R,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> run(R (*method)(P1, P2, P3), A1 a1, A2 a2, A3 a3);


template <typename R,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> run(R (*method)(P1, P2, P3, P4), A1 a1, A2 a2, A3 a3, A4 a4);


template <typename R,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> run(R (*method)(P1, P2, P3, P4, P5),
              A1 a1, A2 a2, A3 a3, A4 a4, A5 a5);


namespace internal {

template <typename R>
class ThunkProcess : public Process<ThunkProcess<R> >
{
public:
  ThunkProcess(const std::tr1::function<R(void)>& _thunk,
               const Promise<R>& _promise)
    : thunk(_thunk),
      promise(_promise) {}

  virtual ~ThunkProcess() {}

protected:
  virtual void operator () ()
  {
    promise.set(thunk());
  }

private:
  std::tr1::function<R(void)> thunk;
  Promise<R> promise;
};

} // namespace internal {


template <typename R>
Future<R> run(R (*method)())
{
  std::tr1::function<R(void)> thunk =
    std::tr1::bind(method);

  Promise<R> promise;

  spawn(new internal::ThunkProcess<R>(thunk, promise), true);

  return promise.future();
}


template <typename R, typename P1, typename A1>
Future<R> run(R (*method)(P1), A1 a1)
{
  std::tr1::function<R(void)> thunk =
    std::tr1::bind(method, a1);

  Promise<R> promise;

  spawn(new internal::ThunkProcess<R>(thunk, promise), true);

  return promise.future();
}


template <typename R,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> run(R (*method)(P1, P2), A1 a1, A2 a2)
{
  std::tr1::function<R(void)> thunk =
    std::tr1::bind(method, a1, a2);

  Promise<R> promise;

  spawn(new internal::ThunkProcess<R>(thunk, promise), true);

  return promise.future();
}


template <typename R,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> run(R (*method)(P1, P2, P3), A1 a1, A2 a2, A3 a3)
{
  std::tr1::function<R(void)> thunk =
    std::tr1::bind(method, a1, a2, a3);

  Promise<R> promise;

  spawn(new internal::ThunkProcess<R>(thunk, promise), true);

  return promise.future();
}


template <typename R,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> run(R (*method)(P1, P2, P3, P4), A1 a1, A2 a2, A3 a3, A4 a4)
{
  std::tr1::function<R(void)> thunk =
    std::tr1::bind(method, a1, a2, a3, a4);

  Promise<R> promise;

  spawn(new internal::ThunkProcess<R>(thunk, promise), true);

  return promise.future();
}


template <typename R,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> run(R (*method)(P1, P2, P3, P4, P5),
              A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  std::tr1::function<R(void)> thunk =
    std::tr1::bind(method, a1, a2, a3, a4, a5);

  Promise<R> promise;

  spawn(new internal::ThunkProcess<R>(thunk, promise), true);

  return promise.future();
}

} // namespace process {

#endif // __PROCESS_RUN_HPP__
