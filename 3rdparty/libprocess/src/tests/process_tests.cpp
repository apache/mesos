#include <arpa/inet.h>

#include <gmock/gmock.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <string>
#include <sstream>

#include <process/async.hpp>
#include <process/collect.hpp>
#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/executor.hpp>
#include <process/filter.hpp>
#include <process/future.hpp>
#include <process/gc.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/limiter.hpp>
#include <process/process.hpp>
#include <process/run.hpp>
#include <process/time.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/stopwatch.hpp>

#include "encoder.hpp"

using namespace process;

using std::string;

using testing::_;
using testing::Assign;
using testing::DoAll;
using testing::Return;
using testing::ReturnArg;

// TODO(bmahler): Move tests into their own files as appropriate.

TEST(Process, event)
{
  Event* event = new TerminateEvent(UPID());
  EXPECT_FALSE(event->is<MessageEvent>());
  EXPECT_FALSE(event->is<ExitedEvent>());
  EXPECT_TRUE(event->is<TerminateEvent>());
  delete event;
}


TEST(Process, future)
{
  Promise<bool> promise;
  promise.set(true);
  ASSERT_TRUE(promise.future().isReady());
  EXPECT_TRUE(promise.future().get());
}


TEST(Process, associate)
{
  Promise<bool> promise1;
  Future<bool> future1(true);
  promise1.associate(future1);
  ASSERT_TRUE(promise1.future().isReady());
  EXPECT_TRUE(promise1.future().get());

  Promise<bool> promise2;
  Future<bool> future2;
  promise2.associate(future2);
  future2.discard();
  ASSERT_TRUE(promise2.future().isDiscarded());

  Promise<bool> promise3;
  Promise<bool> promise4;
  promise3.associate(promise4.future());
  promise4.fail("associate");
  ASSERT_TRUE(promise3.future().isFailed());
  EXPECT_EQ("associate", promise3.future().failure());

  // Test that 'discard' is associated in both directions.
  Promise<bool> promise5;
  Future<bool> future3;
  promise5.associate(future3);
  EXPECT_FALSE(future3.isDiscarded());
  promise5.future().discard();
  EXPECT_TRUE(future3.isDiscarded());
}


void onAny(const Future<bool>& future, bool* b)
{
  ASSERT_TRUE(future.isReady());
  *b = future.get();
}


TEST(Process, onAny)
{
  bool b = false;
  Future<bool>(true)
    .onAny(lambda::bind(&onAny, lambda::_1, &b));
  EXPECT_TRUE(b);
}


Future<string> itoa1(int* const& i)
{
  std::ostringstream out;
  out << *i;
  return out.str();
}


string itoa2(int* const& i)
{
  std::ostringstream out;
  out << *i;
  return out.str();
}


TEST(Process, then)
{
  Promise<int*> promise;

  int i = 42;

  promise.set(&i);

  Future<string> future = promise.future()
    .then(lambda::bind(&itoa1, lambda::_1));

  ASSERT_TRUE(future.isReady());
  EXPECT_EQ("42", future.get());

  future = promise.future()
    .then(lambda::bind(&itoa2, lambda::_1));

  ASSERT_TRUE(future.isReady());
  EXPECT_EQ("42", future.get());
}


Future<bool> readyFuture()
{
  return true;
}


Future<bool> failedFuture()
{
  return Failure("The value is not positive (or zero)");
}


Future<bool> pendingFuture(Future<bool>* future)
{
  return *future; // Keep it pending.
}


Future<string> second(const bool& b)
{
  return b ? string("true") : string("false");
}


Future<string> third(const string& s)
{
  return s;
}


TEST(Process, chain)
{
  Promise<int*> promise;

  Future<string> s = readyFuture()
    .then(lambda::bind(&second, lambda::_1))
    .then(lambda::bind(&third, lambda::_1));

  s.await();

  ASSERT_TRUE(s.isReady());
  EXPECT_EQ("true", s.get());

  s = failedFuture()
    .then(lambda::bind(&second, lambda::_1))
    .then(lambda::bind(&third, lambda::_1));

  s.await();

  ASSERT_TRUE(s.isFailed());

  Future<bool> future;

  s = pendingFuture(&future)
    .then(lambda::bind(&second, lambda::_1))
    .then(lambda::bind(&third, lambda::_1));

  ASSERT_TRUE(s.isPending());
  ASSERT_TRUE(future.isPending());

  s.discard();

  future.await();

  ASSERT_TRUE(future.isDiscarded());
}


class SpawnProcess : public Process<SpawnProcess>
{
public:
  MOCK_METHOD0(initialize, void(void));
  MOCK_METHOD0(finalize, void(void));
};


TEST(Process, spawn)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  SpawnProcess process;

  EXPECT_CALL(process, initialize())
    .Times(1);

  EXPECT_CALL(process, finalize())
    .Times(1);

  PID<SpawnProcess> pid = spawn(process);

  ASSERT_FALSE(!pid);

  ASSERT_FALSE(wait(pid, Seconds(0)));

  terminate(pid);
  wait(pid);
}


class DispatchProcess : public Process<DispatchProcess>
{
public:
  MOCK_METHOD0(func0, void(void));
  MOCK_METHOD1(func1, bool(bool));
  MOCK_METHOD1(func2, Future<bool>(bool));
  MOCK_METHOD1(func3, int(int));
  MOCK_METHOD2(func4, Future<bool>(bool, int));
};


TEST(Process, dispatch)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DispatchProcess process;

  EXPECT_CALL(process, func0())
    .Times(1);

  EXPECT_CALL(process, func1(_))
    .WillOnce(ReturnArg<0>());

  EXPECT_CALL(process, func2(_))
    .WillOnce(ReturnArg<0>());

  PID<DispatchProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  dispatch(pid, &DispatchProcess::func0);

  Future<bool> future;

  future = dispatch(pid, &DispatchProcess::func1, true);

  EXPECT_TRUE(future.get());

  future = dispatch(pid, &DispatchProcess::func2, true);

  EXPECT_TRUE(future.get());

  terminate(pid);
  wait(pid);
}


TEST(Process, defer1)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DispatchProcess process;

  EXPECT_CALL(process, func0())
    .Times(1);

  EXPECT_CALL(process, func1(_))
    .WillOnce(ReturnArg<0>());

  EXPECT_CALL(process, func2(_))
    .WillOnce(ReturnArg<0>());

  EXPECT_CALL(process, func4(_, _))
    .WillRepeatedly(ReturnArg<0>());

  PID<DispatchProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  {
    Deferred<void(void)> func0 =
      defer(pid, &DispatchProcess::func0);
    func0();
  }

  Future<bool> future;

  {
    Deferred<Future<bool>(void)> func1 =
      defer(pid, &DispatchProcess::func1, true);
    future = func1();
    EXPECT_TRUE(future.get());
  }

  {
    Deferred<Future<bool>(void)> func2 =
      defer(pid, &DispatchProcess::func2, true);
    future = func2();
    EXPECT_TRUE(future.get());
  }

  {
    Deferred<Future<bool>(void)> func4 =
      defer(pid, &DispatchProcess::func4, true, 42);
    future = func4();
    EXPECT_TRUE(future.get());
  }

  {
    Deferred<Future<bool>(bool)> func4 =
      defer(pid, &DispatchProcess::func4, lambda::_1, 42);
    future = func4(false);
    EXPECT_FALSE(future.get());
  }

  {
    Deferred<Future<bool>(int)> func4 =
      defer(pid, &DispatchProcess::func4, true, lambda::_1);
    future = func4(42);
    EXPECT_TRUE(future.get());
  }

  // Only take const &!

  terminate(pid);
  wait(pid);
}


class DeferProcess : public Process<DeferProcess>
{
public:
  Future<string> func1(const Future<int>& f)
  {
    return f.then(defer(self(), &Self::_func1, lambda::_1));
  }

  Future<string> func2(const Future<int>& f)
  {
    return f.then(defer(self(), &Self::_func2));
  }

private:
  Future<string> _func1(int i)
  {
    return stringify(i);
  }

  Future<string> _func2()
  {
    return string("42");
  }
};


TEST(Process, defer2)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DeferProcess process;

  PID<DeferProcess> pid = spawn(process);

  Future<string> f = dispatch(pid, &DeferProcess::func1, 41);

  f.await();

  ASSERT_TRUE(f.isReady());
  EXPECT_EQ("41", f.get());

  f = dispatch(pid, &DeferProcess::func2, 41);

  f.await();

  ASSERT_TRUE(f.isReady());
  EXPECT_EQ("42", f.get());

  terminate(pid);
  wait(pid);
}


template <typename T>
void set(T* t1, const T& t2)
{
  *t1 = t2;
}


TEST(Process, defer3)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  volatile bool bool1 = false;
  volatile bool bool2 = false;

  Deferred<void(bool)> set1 =
#if __cplusplus >= 201103L
    defer([&bool1] (bool b) { bool1 = b; });
#else // __cplusplus >= 201103L
    defer(std::tr1::function<void(bool)>(
              std::tr1::bind(&set<volatile bool>,
                             &bool1,
                             std::tr1::placeholders::_1)));
#endif // __cplusplus >= 201103L

  set1(true);

  Deferred<void(bool)> set2 =
#if __cplusplus >= 201103L
    defer([&bool2] (bool b) { bool2 = b; });
#else // __cplusplus >= 201103L
    defer(std::tr1::function<void(bool)>(
              std::tr1::bind(&set<volatile bool>,
                             &bool2,
                             std::tr1::placeholders::_1)));
#endif // __cplusplus >= 201103L

  set2(true);

  while (!bool1);
  while (!bool2);
}


class HandlersProcess : public Process<HandlersProcess>
{
public:
  HandlersProcess()
  {
    install("func", &HandlersProcess::func);
  }

  MOCK_METHOD2(func, void(const UPID&, const string&));
};


TEST(Process, handlers)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HandlersProcess process;

  EXPECT_CALL(process, func(_, _))
    .Times(1);

  PID<HandlersProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  post(pid, "func");

  terminate(pid, false);
  wait(pid);
}


// Tests EXPECT_MESSAGE and EXPECT_DISPATCH and in particular that an
// event can get dropped before being processed.
TEST(Process, expect)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HandlersProcess process;

  EXPECT_CALL(process, func(_, _))
    .Times(0);

  PID<HandlersProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  Future<Message> message = DROP_MESSAGE("func", _, _);

  post(pid, "func");

  AWAIT_EXPECT_READY(message);

  Future<Nothing> func = DROP_DISPATCH(pid, &HandlersProcess::func);

  dispatch(pid, &HandlersProcess::func, pid, "");

  AWAIT_EXPECT_READY(func);

  terminate(pid, false);
  wait(pid);
}


// Tests the FutureArg<N> action.
TEST(Process, action)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HandlersProcess process;

  PID<HandlersProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  Future<string> future1;
  Future<Nothing> future2;
  EXPECT_CALL(process, func(_, _))
    .WillOnce(FutureArg<1>(&future1))
    .WillOnce(FutureSatisfy(&future2));

  dispatch(pid, &HandlersProcess::func, pid, "hello world");

  AWAIT_EXPECT_EQ("hello world", future1);

  EXPECT_TRUE(future2.isPending());

  dispatch(pid, &HandlersProcess::func, pid, "hello world");

  AWAIT_EXPECT_READY(future2);

  terminate(pid, false);
  wait(pid);
}


class BaseProcess : public Process<BaseProcess>
{
public:
  virtual void func() = 0;
  MOCK_METHOD0(foo, void());
};


class DerivedProcess : public BaseProcess
{
public:
  DerivedProcess() {}
  MOCK_METHOD0(func, void());
};


TEST(Process, inheritance)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DerivedProcess process;

  EXPECT_CALL(process, func())
    .Times(2);

  EXPECT_CALL(process, foo())
    .Times(1);

  PID<DerivedProcess> pid1 = spawn(&process);

  ASSERT_FALSE(!pid1);

  dispatch(pid1, &DerivedProcess::func);

  PID<BaseProcess> pid2(process);
  PID<BaseProcess> pid3 = pid1;

  ASSERT_EQ(pid2, pid3);

  dispatch(pid3, &BaseProcess::func);
  dispatch(pid3, &BaseProcess::foo);

  terminate(pid1, false);
  wait(pid1);
}


TEST(Process, thunk)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  struct Thunk
  {
    static int run(int i)
    {
      return i;
    }

    static int run(int i, int j)
    {
      return run(i + j);
    }
  };

  int result = run(&Thunk::run, 21, 21).get();

  EXPECT_EQ(42, result);
}


class DelegatorProcess : public Process<DelegatorProcess>
{
public:
  DelegatorProcess(const UPID& delegatee)
  {
    delegate("func", delegatee);
  }
};


class DelegateeProcess : public Process<DelegateeProcess>
{
public:
  DelegateeProcess()
  {
    install("func", &DelegateeProcess::func);
  }

  MOCK_METHOD2(func, void(const UPID&, const string&));
};


TEST(Process, delegate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DelegateeProcess delegatee;
  DelegatorProcess delegator(delegatee.self());

  EXPECT_CALL(delegatee, func(_, _))
    .Times(1);

  spawn(&delegator);
  spawn(&delegatee);

  post(delegator.self(), "func");

  terminate(delegator, false);
  wait(delegator);

  terminate(delegatee, false);
  wait(delegatee);
}


class TimeoutProcess : public Process<TimeoutProcess>
{
public:
  MOCK_METHOD0(timeout, void());
};


TEST(Process, delay)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  volatile bool timeoutCalled = false;

  TimeoutProcess process;

  EXPECT_CALL(process, timeout())
    .WillOnce(Assign(&timeoutCalled, true));

  spawn(process);

  delay(Seconds(5), process.self(), &TimeoutProcess::timeout);

  Clock::advance(Seconds(5));

  while (!timeoutCalled);

  terminate(process);
  wait(process);

  Clock::resume();
}


class OrderProcess : public Process<OrderProcess>
{
public:
  void order(const PID<TimeoutProcess>& pid)
  {
    // TODO(benh): Add a test which uses 'send' instead of dispatch.
    dispatch(pid, &TimeoutProcess::timeout);
  }
};


TEST(Process, order)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  TimeoutProcess process1;

  volatile bool timeoutCalled = false;

  EXPECT_CALL(process1, timeout())
    .WillOnce(Assign(&timeoutCalled, true));

  spawn(process1);

  Time now = Clock::now(&process1);

  Seconds seconds(1);

  Clock::advance(Seconds(1));

  EXPECT_EQ(now, Clock::now(&process1));

  OrderProcess process2;
  spawn(process2);

  dispatch(process2, &OrderProcess::order, process1.self());

  while (!timeoutCalled);

  EXPECT_EQ(now + seconds, Clock::now(&process1));

  terminate(process1);
  wait(process1);

  terminate(process2);
  wait(process2);

  Clock::resume();
}


class DonateProcess : public Process<DonateProcess>
{
public:
  void donate()
  {
    DonateProcess process;
    spawn(process);
    terminate(process);
    wait(process);
  }
};


TEST(Process, donate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DonateProcess process;
  spawn(process);

  dispatch(process, &DonateProcess::donate);

  terminate(process, false);
  wait(process);
}


class ExitedProcess : public Process<ExitedProcess>
{
public:
  ExitedProcess(const UPID& pid) { link(pid); }

  MOCK_METHOD1(exited, void(const UPID&));
};


TEST(Process, exited)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  UPID pid = spawn(new ProcessBase(), true);

  ExitedProcess process(pid);

  volatile bool exitedCalled = false;

  EXPECT_CALL(process, exited(pid))
    .WillOnce(Assign(&exitedCalled, true));

  spawn(process);

  terminate(pid);

  while (!exitedCalled);

  terminate(process);
  wait(process);
}


TEST(Process, select)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Promise<int> promise1;
  Promise<int> promise2;
  Promise<int> promise3;
  Promise<int> promise4;

  std::set<Future<int> > futures;
  futures.insert(promise1.future());
  futures.insert(promise2.future());
  futures.insert(promise3.future());
  futures.insert(promise4.future());

  promise1.set(42);

  Future<Future<int> > future = select(futures);

  EXPECT_TRUE(future.await());
  EXPECT_TRUE(future.isReady());
  EXPECT_TRUE(future.get().isReady());
  EXPECT_EQ(42, future.get().get());
}


TEST(Process, collect)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  // First ensure an empty list functions correctly.
  std::list<Future<int> > empty;
  Future<std::list<int> > future = collect(empty);
  AWAIT_ASSERT_READY(future);
  EXPECT_TRUE(future.get().empty());

  Promise<int> promise1;
  Promise<int> promise2;
  Promise<int> promise3;
  Promise<int> promise4;

  std::list<Future<int> > futures;
  futures.push_back(promise1.future());
  futures.push_back(promise2.future());
  futures.push_back(promise3.future());
  futures.push_back(promise4.future());

  // Set them out-of-order.
  promise4.set(4);
  promise2.set(2);
  promise1.set(1);
  promise3.set(3);

  future = collect(futures);

  AWAIT_ASSERT_READY(future);

  std::list<int> values;
  values.push_back(1);
  values.push_back(2);
  values.push_back(3);
  values.push_back(4);

  // We expect them to be returned in the same order as the
  // future list that was passed in.
  EXPECT_EQ(values, future.get());
}


TEST(Process, await)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  // First ensure an empty list functions correctly.
  std::list<Future<int> > empty;
  Future<std::list<Future<int> > > future = await(empty);
  AWAIT_ASSERT_READY(future);
  EXPECT_TRUE(future.get().empty());

  Promise<int> promise1;
  Promise<int> promise2;
  Promise<int> promise3;
  Promise<int> promise4;

  std::list<Future<int> > futures;
  futures.push_back(promise1.future());
  futures.push_back(promise2.future());
  futures.push_back(promise3.future());
  futures.push_back(promise4.future());

  // Set them out-of-order.
  promise4.set(4);
  promise2.set(2);
  promise1.set(1);
  promise3.set(3);

  future = await(futures);

  AWAIT_ASSERT_READY(future);

  EXPECT_EQ(futures.size(), future.get().size());

  // We expect them to be returned in the same order as the
  // future list that was passed in.
  int i = 1;
  foreach (const Future<int>& result, future.get()) {
    ASSERT_TRUE(result.isReady());
    ASSERT_EQ(i++, result.get());
  }
}


class SettleProcess : public Process<SettleProcess>
{
public:
  SettleProcess() : calledDispatch(false) {}

  virtual void initialize()
  {
    os::sleep(Milliseconds(10));
    delay(Seconds(0), self(), &SettleProcess::afterDelay);
  }

  void afterDelay()
  {
    dispatch(self(), &SettleProcess::afterDispatch);
    os::sleep(Milliseconds(10));
    TimeoutProcess timeoutProcess;
    spawn(timeoutProcess);
    terminate(timeoutProcess);
    wait(timeoutProcess);
  }

  void afterDispatch()
  {
    os::sleep(Milliseconds(10));
    calledDispatch = true;
  }

  volatile bool calledDispatch;
};


TEST(Process, settle)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();
  SettleProcess process;
  spawn(process);
  Clock::settle();
  ASSERT_TRUE(process.calledDispatch);
  terminate(process);
  wait(process);
  Clock::resume();
}


TEST(Process, pid)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  TimeoutProcess process;

  PID<TimeoutProcess> pid = process;
}


class Listener1 : public Process<Listener1>
{
public:
  virtual void event1() = 0;
};


class Listener2 : public Process<Listener2>
{
public:
  virtual void event2() = 0;
};


class MultipleListenerProcess
  : public Process<MultipleListenerProcess>,
    public Listener1,
    public Listener2
{
public:
  MOCK_METHOD0(event1, void());
  MOCK_METHOD0(event2, void());
};


TEST(Process, listener)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  MultipleListenerProcess process;

  EXPECT_CALL(process, event1())
    .Times(1);

  EXPECT_CALL(process, event2())
    .Times(1);

  spawn(process);

  dispatch(PID<Listener1>(process), &Listener1::event1);
  dispatch(PID<Listener2>(process), &Listener2::event2);

  terminate(process, false);
  wait(process);
}


class EventReceiver
{
public:
  MOCK_METHOD1(event1, void(int));
  MOCK_METHOD1(event2, void(const string&));
};


TEST(Process, executor)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  volatile bool event1Called = false;
  volatile bool event2Called = false;

  EventReceiver receiver;

  EXPECT_CALL(receiver, event1(42))
    .WillOnce(Assign(&event1Called, true));

  EXPECT_CALL(receiver, event2("event2"))
    .WillOnce(Assign(&event2Called, true));

  Executor executor;

  Deferred<void(int)> event1 =
    executor.defer(lambda::function<void(int)>(
                       lambda::bind(&EventReceiver::event1,
                                    &receiver,
                                    lambda::_1)));
  event1(42);

  Deferred<void(const string&)> event2 =
    executor.defer(lambda::function<void(const string&)>(
                       lambda::bind(&EventReceiver::event2,
                                    &receiver,
                                    lambda::_1)));

  event2("event2");

  while (!event1Called);
  while (!event2Called);
}


class RemoteProcess : public Process<RemoteProcess>
{
public:
  RemoteProcess()
  {
    install("handler", &RemoteProcess::handler);
  }

  MOCK_METHOD2(handler, void(const UPID&, const string&));
};


TEST(Process, remote)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  RemoteProcess process;

  volatile bool handlerCalled = false;

  EXPECT_CALL(process, handler(_, _))
    .WillOnce(Assign(&handlerCalled, true));

  spawn(process);

  int s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

  ASSERT_LE(0, s);

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = PF_INET;
  addr.sin_port = htons(process.self().port);
  addr.sin_addr.s_addr = process.self().ip;

  ASSERT_EQ(0, connect(s, (sockaddr*) &addr, sizeof(addr)));

  Message message;
  message.name = "handler";
  message.from = UPID();
  message.to = process.self();

  const string& data = MessageEncoder::encode(&message);

  ASSERT_EQ(data.size(), write(s, data.data(), data.size()));

  ASSERT_EQ(0, close(s));

  while (!handlerCalled);

  terminate(process);
  wait(process);
}


int foo()
{
  return 1;
}

int foo1(int a)
{
  return a;
}


int foo2(int a, int b)
{
  return a + b;
}


int foo3(int a, int b, int c)
{
  return a + b + c;
}


int foo4(int a, int b, int c, int d)
{
  return a + b + c + d;
}


void bar(int a)
{
  return;
}


TEST(Process, async)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  // Non-void functions with different no.of args.
  EXPECT_EQ(1, async(&foo).get());
  EXPECT_EQ(10, async(&foo1, 10).get());
  EXPECT_EQ(30, async(&foo2, 10, 20).get());
  EXPECT_EQ(60, async(&foo3, 10, 20, 30).get());
  EXPECT_EQ(100, async(&foo4, 10, 20, 30, 40).get());

  // Non-void function with a complex arg.
  int i = 42;
  EXPECT_EQ("42", async(&itoa2, &i).get());

  // Non-void function that returns a future.
  EXPECT_EQ("42", async(&itoa1, &i).get().get());
}


TEST(Process, limiter)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  int permits = 2;
  Duration duration = Milliseconds(5);

  RateLimiter limiter(permits, duration);
  Milliseconds interval = duration / permits;

  Stopwatch stopwatch;
  stopwatch.start();

  Future<Nothing> acquire1 = limiter.acquire();
  Future<Nothing> acquire2 = limiter.acquire();
  Future<Nothing> acquire3 = limiter.acquire();

  AWAIT_READY(acquire1);

  AWAIT_READY(acquire2);
  ASSERT_LE(interval, stopwatch.elapsed());

  AWAIT_READY(acquire3);
  ASSERT_LE(interval * 2, stopwatch.elapsed());
}


class FileServer : public Process<FileServer>
{
public:
  FileServer(const string& _path)
    : path(_path) {}

  virtual void initialize()
  {
    provide("", path);
  }

  const string path;
};


TEST(Process, provide)
{
  const Try<string>& mkdtemp = os::mkdtemp();
  ASSERT_SOME(mkdtemp);

  const string LOREM_IPSUM =
      "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
      "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad "
      "minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip "
      "ex ea commodo consequat. Duis aute irure dolor in reprehenderit in "
      "voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur "
      "sint occaecat cupidatat non proident, sunt in culpa qui officia "
      "deserunt mollit anim id est laborum.";

  const string path = path::join(mkdtemp.get(), "lorem.txt");
  ASSERT_SOME(os::write(path, LOREM_IPSUM));

  FileServer server(path);
  PID<FileServer> pid = spawn(server);

  Future<http::Response> response = http::get(pid);

  AWAIT_READY(response);

  ASSERT_EQ(LOREM_IPSUM, response.get().body);

  terminate(server);
  wait(server);

  ASSERT_SOME(os::rmdir(path));
}


#if __cplusplus >= 201103L
int baz(string s) { return 42; }

Future<int> bam(string s) { return 42; }


TEST(Process, defers)
{
  {
    std::function<Future<int>(string)> f =
      defer(std::bind(baz, std::placeholders::_1));

    Deferred<Future<int>(string)> d =
      defer(std::bind(baz, std::placeholders::_1));

    Future<int> future = Future<string>().then(
        defer(std::bind(baz, std::placeholders::_1)));

    Future<int> future3 = Future<string>().then(
        std::bind(baz, std::placeholders::_1));

    Future<string>().then(std::function<int(string)>());
    Future<string>().then(std::function<int(void)>());

    Future<int> future11 = Future<string>().then(
        defer(std::bind(bam, std::placeholders::_1)));

    Future<int> future12 = Future<string>().then(
        std::bind(bam, std::placeholders::_1));

    std::function<Future<int>(string)> f2 =
      defer([] (string s) { return baz(s); });

    Deferred<Future<int>(string)> d2 =
      defer([] (string s) { return baz(s); });

    Future<int> future2 = Future<string>().then(
        defer([] (string s) { return baz(s); }));

    Future<int> future4 = Future<string>().then(
        [] (string s) { return baz(s); });

    Future<int> future5 = Future<string>().then(
        defer([] (string s) -> Future<int> { return baz(s); }));

    Future<int> future6 = Future<string>().then(
        defer([] (string s) { return Future<int>(baz(s)); }));

    Future<int> future7 = Future<string>().then(
        defer([] (string s) { return bam(s); }));

    Future<int> future8 = Future<string>().then(
        [] (string s) { return Future<int>(baz(s)); });

    Future<int> future9 = Future<string>().then(
        [] (string s) -> Future<int> { return baz(s); });

    Future<int> future10 = Future<string>().then(
        [] (string s) { return bam(s); });
  }

//   {
//     // CAN NOT DO IN CLANG!
//     std::function<void(string)> f =
//       defer(std::bind(baz, std::placeholders::_1));

//     std::function<int(string)> blah;
//     std::function<void(string)> blam = blah;

//     std::function<void(string)> f2 =
//       defer([] (string s) { return baz(s); });
//   }

//   {
//     // CAN NOT DO WITH GCC OR CLANG!
//     std::function<int(int)> f =
//       defer(std::bind(baz, std::placeholders::_1));
//   }

  {
    std::function<Future<int>(void)> f =
      defer(std::bind(baz, "42"));

    std::function<Future<int>(void)> f2 =
      defer([] () { return baz("42"); });
  }

  {
    std::function<Future<int>(int)> f =
      defer(std::bind(baz, "42"));

    std::function<Future<int>(int)> f2 =
      defer([] (int i) { return baz("42"); });
  }

  // Don't care about value passed from Future::then.
  {
    Future<int> future = Future<string>().then(
        defer(std::bind(baz, "42")));

    Future<int> future3 = Future<string>().then(
        std::bind(baz, "42"));

    Future<int> future11 = Future<string>().then(
        defer(std::bind(bam, "42")));

    Future<int> future12 = Future<string>().then(
        std::bind(bam, "42"));

    Future<int> future2 = Future<string>().then(
        defer([] () { return baz("42"); }));

    Future<int> future4 = Future<string>().then(
        [] () { return baz("42"); });

    Future<int> future5 = Future<string>().then(
        defer([] () -> Future<int> { return baz("42"); }));

    Future<int> future6 = Future<string>().then(
        defer([] () { return Future<int>(baz("42")); }));

    Future<int> future7 = Future<string>().then(
        defer([] () { return bam("42"); }));

    Future<int> future8 = Future<string>().then(
        [] () { return Future<int>(baz("42")); });

    Future<int> future9 = Future<string>().then(
        [] () -> Future<int> { return baz("42"); });

    Future<int> future10 = Future<string>().then(
        [] () { return bam("42"); });
  }

  struct Functor
  {
    int operator () (string) const { return 42; }
    int operator () () const { return 42; }
  } functor;

  Future<int> future13 = Future<string>().then(
      defer(functor));
}
#endif // __cplusplus >= 201103L
