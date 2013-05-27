#ifndef __ASYNC_HPP__
#define __ASYNC_HPP__

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <tr1/functional>

namespace process {

// TODO(vinod): Merge this into ExecutorProcess.
// TODO(vinod): Add support for void functions. Currently this is tricky,
// because Future<void> is not supported.
class AsyncExecutorProcess : public Process<AsyncExecutorProcess>
{
private:
  friend class AsyncExecutor;

  AsyncExecutorProcess() : ProcessBase(ID::generate("__async_executor__")) {}
  virtual ~AsyncExecutorProcess() {}

  // Not copyable, not assignable.
  AsyncExecutorProcess(const AsyncExecutorProcess&);
  AsyncExecutorProcess& operator = (const AsyncExecutorProcess&);

  template<typename F>
  typename std::tr1::result_of<F(void)>::type execute(
      const F& f)
  {
    terminate(self()); // Terminate this process after the function returns.
    return f();
  }

  // TODO(vinod): Use boost macro enumerations.
  template<typename F, typename A1>
  typename std::tr1::result_of<F(A1)>::type execute(
      const F& f, A1 a1)
  {
    terminate(self()); // Terminate this process after the function returns.
    return f(a1);
  }

  template<typename F, typename A1, typename A2>
  typename std::tr1::result_of<F(A1, A2)>::type execute(
      const F& f, A1 a1, A2 a2)
  {
    terminate(self()); // Terminate this process after the function returns.
    return f(a1, a2);
  }

  template<typename F, typename A1, typename A2, typename A3>
  typename std::tr1::result_of<F(A1, A2, A3)>::type execute(
      const F& f, A1 a1, A2 a2, A3 a3)
  {
    terminate(self()); // Terminate this process after the function returns.
    return f(a1, a2, a3);
  }

  template<typename F, typename A1, typename A2, typename A3, typename A4>
  typename std::tr1::result_of<F(A1, A2, A3, A4)>::type execute(
      const F& f, A1 a1, A2 a2, A3 a3, A4 a4)
  {
    terminate(self()); // Terminate this process after the function returns.
    return f(a1, a2, a3, a4);
  }
};


// This is a wrapper around AsyncExecutorProcess.
class AsyncExecutor
{
private:
  // Declare async functions as friends.
  template<typename F>
  friend Future<typename std::tr1::result_of<F(void)>::type> async(
      const F& f);

  template<typename F, typename A1>
  friend Future<typename std::tr1::result_of<F(A1)>::type> async(
      const F& f, A1 a1);

  template<typename F, typename A1, typename A2>
  friend Future<typename std::tr1::result_of<F(A1, A2)>::type> async(
      const F& f, A1 a1, A2 a2);

  template<typename F, typename A1, typename A2, typename A3>
  friend Future<typename std::tr1::result_of<F(A1, A2, A3)>::type> async(
      const F& f, A1 a1, A2 a2, A3 a3);

  template<typename F, typename A1, typename A2, typename A3, typename A4>
  friend Future<typename std::tr1::result_of<F(A1, A2, A3, A4)>::type> async(
      const F& f, A1 a1, A2 a2, A3 a3, A4 a4);

  AsyncExecutor()
  {
    process = new AsyncExecutorProcess();
    spawn(process, true); // Automatically GC.
  }

  virtual ~AsyncExecutor() {}

  // Not copyable, not assignable.
  AsyncExecutor(const AsyncExecutor&);
  AsyncExecutor& operator = (const AsyncExecutor&);

  template<typename F>
  Future<typename std::tr1::result_of<F(void)>::type> execute(
      const F& f)
  {
    // Necessary to disambiguate.
    typedef typename std::tr1::result_of<F(void)>::type
        (AsyncExecutorProcess::*R)(const F&);

    return dispatch(process,
                    static_cast<R>(&AsyncExecutorProcess::execute),
                    f);
  }

  // TODO(vinod): Use boost macro enumerations.
  template<typename F, typename A1>
  Future<typename std::tr1::result_of<F(A1)>::type> execute(
      const F& f, A1 a1)
  {
    // Necessary to disambiguate.
    typedef typename std::tr1::result_of<F(A1)>::type
        (AsyncExecutorProcess::*R)(const F&, A1);

    return dispatch(process,
                    static_cast<R>(&AsyncExecutorProcess::execute),
                    f,
                    a1);
  }

  template<typename F, typename A1, typename A2>
  Future<typename std::tr1::result_of<F(A1, A2)>::type> execute(
      const F& f, A1 a1, A2 a2)
  {
    // Necessary to disambiguate.
    typedef typename std::tr1::result_of<F(A1, A2)>::type
        (AsyncExecutorProcess::*R)(const F&, A1, A2);

    return dispatch(process,
                    static_cast<R>(&AsyncExecutorProcess::execute),
                    f,
                    a1,
                    a2);
  }

  template<typename F, typename A1, typename A2, typename A3>
  Future<typename std::tr1::result_of<F(A1, A2, A3)>::type> execute(
      const F& f, A1 a1, A2 a2, A3 a3)
  {
    // Necessary to disambiguate.
    typedef typename std::tr1::result_of<F(A1, A2, A3)>::type
        (AsyncExecutorProcess::*R)(const F&, A1, A2, A3);

    return dispatch(process,
                    static_cast<R>(&AsyncExecutorProcess::execute),
                    f,
                    a1,
                    a2,
                    a3);
  }

  template<typename F, typename A1, typename A2, typename A3, typename A4>
  Future<typename std::tr1::result_of<F(A1, A2, A3, A4)>::type> execute(
      const F& f, A1 a1, A2 a2, A3 a3, A4 a4)
  {
    // Necessary to disambiguate.
    typedef typename std::tr1::result_of<F(A1, A2, A3, A4)>::type
        (AsyncExecutorProcess::*R)(const F&, A1, A2, A3, A4);

    return dispatch(process,
                    static_cast<R>(&AsyncExecutorProcess::execute),
                    f,
                    a1,
                    a2,
                    a3,
                    a4);
  }

  AsyncExecutorProcess* process;
};


// Provides an abstraction for asynchronously executing a function.
// TODO(vinod): Use boost macro to enumerate arguments/params.
template<typename F>
Future<typename std::tr1::result_of<F(void)>::type>
    async(const F& f)
{
  return AsyncExecutor().execute(f);
}


template<typename F, typename A1>
Future<typename std::tr1::result_of<F(A1)>::type>
    async(const F& f, A1 a1)
{
  return AsyncExecutor().execute(f, a1);
}


template<typename F, typename A1, typename A2>
Future<typename std::tr1::result_of<F(A1, A2)>::type>
    async(const F& f, A1 a1, A2 a2)
{
  return AsyncExecutor().execute(f, a1, a2);
}


template<typename F, typename A1, typename A2, typename A3>
Future<typename std::tr1::result_of<F(A1, A2, A3)>::type>
    async(const F& f, A1 a1, A2 a2, A3 a3)
{
  return AsyncExecutor().execute(f, a1, a2, a3);
}


template<typename F, typename A1, typename A2, typename A3, typename A4>
Future<typename std::tr1::result_of<F(A1, A2, A3, A4)>::type>
    async(const F& f, A1 a1, A2 a2, A3 a3, A4 a4)
{
  return AsyncExecutor().execute(f, a1, a2, a3, a4);
}

} // namespace process {

#endif // __ASYNC_HPP__
