#include <gmock/gmock.h>

#include <string>

#include <process/async.hpp>
#include <process/dispatch.hpp>
#include <process/latch.hpp>
#include <process/process.hpp>
#include <process/run.hpp>
#include <process/timer.hpp>

// Definition of a Set action to be used with gmock.
ACTION_P2(Set, variable, value) { *variable = value; }

using namespace process;

using testing::_;
using testing::ReturnArg;


class SpawnMockProcess : public Process<SpawnMockProcess>
{
public:
  MOCK_METHOD0(__operator_call__, void());
  virtual void operator () () { __operator_call__(); }
};


TEST(libprocess, spawn)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  SpawnMockProcess process;

  EXPECT_CALL(process, __operator_call__())
    .Times(1);

  PID<SpawnMockProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  wait(pid);
}


class DispatchMockProcess : public Process<DispatchMockProcess>
{
public:
  MOCK_METHOD0(func0, void());
  MOCK_METHOD1(func1, bool(bool));
  MOCK_METHOD1(func2, Promise<bool>(bool));
  MOCK_METHOD1(func3, int(int));
  MOCK_METHOD1(func4, Promise<int>(int));
};


TEST(libprocess, dispatch)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DispatchMockProcess process;

  EXPECT_CALL(process, func0())
    .Times(1);

  EXPECT_CALL(process, func1(_))
    .WillOnce(ReturnArg<0>());

  EXPECT_CALL(process, func2(_))
    .WillOnce(ReturnArg<0>());

  PID<DispatchMockProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  dispatch(pid, &DispatchMockProcess::func0);

  Future<bool> future;

  future = dispatch(pid, &DispatchMockProcess::func1, true);

  EXPECT_TRUE(future.get());
  
  future = dispatch(pid, &DispatchMockProcess::func2, true);

  EXPECT_TRUE(future.get());

  post(pid, TERMINATE);
  wait(pid);
}


TEST(libprocess, call)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DispatchMockProcess process;

  EXPECT_CALL(process, func3(_))
    .WillOnce(ReturnArg<0>());

  EXPECT_CALL(process, func4(_))
    .WillOnce(ReturnArg<0>());

  PID<DispatchMockProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  int result;

  result = call(pid, &DispatchMockProcess::func3, 42);

  EXPECT_EQ(42, result);
  
  result = call(pid, &DispatchMockProcess::func4, 43);

  EXPECT_EQ(43, result);

  post(pid, TERMINATE);
  wait(pid);
}


class HandlersMockProcess : public Process<HandlersMockProcess>
{
public:
  HandlersMockProcess()
  {
    installMessageHandler("func", &HandlersMockProcess::func);
  }

  MOCK_METHOD0(func, void());
};


TEST(libprocess, handlers)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HandlersMockProcess process;

  EXPECT_CALL(process, func())
    .Times(1);

  PID<HandlersMockProcess> pid = spawn(&process);

  ASSERT_FALSE(!pid);

  post(pid, "func");

  post(pid, TERMINATE);
  wait(pid);
}


class BaseMockProcess : public Process<BaseMockProcess>
{
public:
  virtual void func() = 0;
  MOCK_METHOD0(foo, void());
};


class DerivedMockProcess : public BaseMockProcess
{
public:
  DerivedMockProcess() {}
  MOCK_METHOD0(func, void());
};


TEST(libprocess, inheritance)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DerivedMockProcess process;

  EXPECT_CALL(process, func())
    .Times(2);

  EXPECT_CALL(process, foo())
    .Times(1);

  PID<DerivedMockProcess> pid1 = spawn(&process);

  ASSERT_FALSE(!pid1);

  dispatch(pid1, &DerivedMockProcess::func);

  PID<BaseMockProcess> pid2(process);
  PID<BaseMockProcess> pid3 = pid1;

  ASSERT_EQ(pid2, pid3);

  dispatch(pid3, &BaseMockProcess::func);
  dispatch(pid3, &BaseMockProcess::foo);

  post(pid1, TERMINATE);
  wait(pid1);
}


TEST(libprocess, thunk)
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
    installMessageHandler("func", &DelegateeProcess::func);
  }

  MOCK_METHOD0(func, void());
};


TEST(libprocess, delegate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DelegateeProcess delegatee;
  DelegatorProcess delegator(delegatee.self());

  EXPECT_CALL(delegatee, func())
    .Times(1);

  spawn(&delegator);
  spawn(&delegatee);

  post(delegator.self(), "func");

  post(delegator.self(), TERMINATE);
  post(delegatee.self(), TERMINATE);

  wait(delegator.self());
  wait(delegatee.self());
}


class TerminateProcess : public Process<TerminateProcess>
{
public:
  TerminateProcess(Latch* _latch) : latch(_latch) {}

protected:
  virtual void operator () ()
  {
    latch->await();
    receive();
    EXPECT_EQ(TERMINATE, name());
  }

private:
  Latch* latch;
};


TEST(libprocess, terminate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Latch latch;

  TerminateProcess process(&latch);

  spawn(&process);

  post(process.self(), "one");
  post(process.self(), "two");
  post(process.self(), "three");

  terminate(process.self());

  latch.trigger();
  
  wait(process.self());
}


class TimeoutProcess : public Process<TimeoutProcess>
{
public:
  TimeoutProcess() {}
  MOCK_METHOD0(timeout, void());
};


TEST(libprocess, DISABLED_timer)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  TimeoutProcess process;

  EXPECT_CALL(process, timeout())
    .Times(1);

  spawn(&process);

  double timeout = 5.0;

  Timer timer =
    delay(timeout, process.self(), &TimeoutProcess::timeout);

  Clock::advance(timeout);

  post(process.self(), TERMINATE);
  wait(process.self());

  Clock::resume();
}


TEST(libprocess, select)
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

  Option<Future<int> > option = select(futures, 0);

  EXPECT_TRUE(option.isSome());
  EXPECT_TRUE(option.get().ready());
  EXPECT_EQ(42, option.get().get());
}


// #define ENUMERATE1(item) item##1
// #define ENUMERATE2(item) ENUMERATE1(item), item##2
// #define ENUMERATE3(item) ENUMERATE2(item), item##3
// #define ENUMERATE4(item) ENUMERATE3(item), item##4
// #define ENUMERATE5(item) ENUMERATE4(item), item##5
// #define ENUMERATE6(item) ENUMERATE5(item), item##6
// #define ENUMERATE(item, n) ENUMERATE##n(item)

// #define GenerateVoidDispatch(n)                                         \
//   template <typename T,                                                 \
//             ENUM(typename P, n),                                        \
//             ENUM(typename A, n)>                                        \
//   void dispatch(const PID<T>& pid,                                      \
//                 void (T::*method)(ENUM(P, n)),                          \
//                 ENUM(A, a, n))                                          \
//   {                                                                     \
//     std::tr1::function<void(T*)> thunk =                                \
//       std::tr1::bind(method, std::tr1::placeholders::_1, ENUM(a, 5));   \
//                                                                         \
//     std::tr1::function<void(ProcessBase*)>* dispatcher =                \
//       new std::tr1::function<void(ProcessBase*)>(                       \
//           std::tr1::bind(&internal::vdispatcher<T>,                     \
//                          std::tr1::placeholders::_1,                    \
//                          thunk));                                       \
//                                                                         \
//     internal::dispatch(pid, dispatcher);                                \
// }

// }


TEST(libprocess, pid)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  TimeoutProcess process;

  PID<TimeoutProcess> pid = process;

//   foo(process, &TimeoutProcess::timeout);
  //  dispatch(process, &TimeoutProcess::timeout);
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


TEST(libprocess, listener)
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
  MOCK_METHOD1(event2, void(const std::string&));
};


TEST(libprocess, Async)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  volatile bool event1Called = false;
  volatile bool event2Called = false;

  EventReceiver receiver;

  EXPECT_CALL(receiver, event1(42))
    .WillOnce(Set(&event1Called, true));

  EXPECT_CALL(receiver, event2("event2"))
    .WillOnce(Set(&event2Called, true));

  async::Dispatch dispatch;

  async::dispatch<void(int)> event1 =
    dispatch(std::tr1::bind(&EventReceiver::event1,
                            &receiver,
                            std::tr1::placeholders::_1));

  event1(42);

  async::dispatch<void(const std::string&)> event2 =
    dispatch(std::tr1::bind(&EventReceiver::event2,
                            &receiver,
                            std::tr1::placeholders::_1));

  event2("event2");

  while (!event1Called);
  while (!event2Called);
}


int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

  return RUN_ALL_TESTS();
}
