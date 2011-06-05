#ifndef __RUN_HPP__
#define __RUN_HPP__

#include <process.hpp>


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
  ThunkProcess(std::tr1::function<R(void)>* _thunk, Future<R>* _future)
    : thunk(_thunk), future(_future) {}

  virtual ~ThunkProcess()
  {
    if (thunk != NULL) {
      delete thunk;
    }

    if (future != NULL) {
      delete future;
    }
  }

protected:
  virtual void operator () ()
  {
    assert(thunk != NULL);
    assert(future != NULL);
    Promise<R>((*thunk)()).associate(*future);
  }

private:
  std::tr1::function<R(void)>* thunk;
  Future<R>* future;
};

} // namespace internal {


template <typename R>
Future<R> run(R (*method)())
{
  std::tr1::function<R(void)>* thunk =
    new std::tr1::function<R(void)>(std::tr1::bind(method));

  Future<R>* future = new Future<R>();

  spawn(new internal::ThunkProcess<R>(thunk, future), true);

  return *future;
}


template <typename R, typename P1, typename A1>
Future<R> run(R (*method)(P1), A1 a1)
{
  std::tr1::function<R(void)>* thunk =
    new std::tr1::function<R(void)>(std::tr1::bind(method, a1));

  Future<R>* future = new Future<R>();

  spawn(new internal::ThunkProcess<R>(thunk, future), true);

  return *future;
}


template <typename R,
          typename P1, typename P2,
          typename A1, typename A2>
Future<R> run(R (*method)(P1, P2), A1 a1, A2 a2)
{
  std::tr1::function<R(void)>* thunk =
    new std::tr1::function<R(void)>(std::tr1::bind(method, a1, a2));

  Future<R>* future = new Future<R>();

  spawn(new internal::ThunkProcess<R>(thunk, future), true);

  return *future;
}


template <typename R,
          typename P1, typename P2, typename P3,
          typename A1, typename A2, typename A3>
Future<R> run(R (*method)(P1, P2, P3), A1 a1, A2 a2, A3 a3)
{
  std::tr1::function<R(void)>* thunk =
    new std::tr1::function<R(void)>(std::tr1::bind(method, a1, a2, a3));

  Future<R>* future = new Future<R>();

  spawn(new internal::ThunkProcess<R>(thunk, future), true);

  return *future;
}


template <typename R,
          typename P1, typename P2, typename P3, typename P4,
          typename A1, typename A2, typename A3, typename A4>
Future<R> run(R (*method)(P1, P2, P3, P4), A1 a1, A2 a2, A3 a3, A4 a4)
{
  std::tr1::function<R(void)>* thunk =
    new std::tr1::function<R(void)>(std::tr1::bind(method, a1, a2, a3, a4));

  Future<R>* future = new Future<R>();

  spawn(new internal::ThunkProcess<R>(thunk, future), true);

  return *future;
}


template <typename R,
          typename P1, typename P2, typename P3, typename P4, typename P5,
          typename A1, typename A2, typename A3, typename A4, typename A5>
Future<R> run(R (*method)(P1, P2, P3, P4, P5),
              A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
{
  std::tr1::function<R(void)>* thunk =
    new std::tr1::function<R(void)>(std::tr1::bind(method, a1, a2, a3, a4, a5));

  Future<R>* future = new Future<R>();

  spawn(new internal::ThunkProcess<R>(thunk, future), true);

  return *future;
}

} // namespace process {

#endif // __RUN_HPP__
