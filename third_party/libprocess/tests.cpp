#include <gmock/gmock.h>

#include <process.hpp>

using process::PID;
using process::Process;
using process::UPID;

using testing::AtMost;


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
    .Times(AtMost(1));

  PID<SpawnMockProcess> pid = process::spawn(&process);

  ASSERT_FALSE(!pid);

  process::wait(pid);
}


class DispatchMockProcess : public Process<DispatchMockProcess>
{
public:
  MOCK_METHOD0(func, void());
  virtual void operator () () { serve(); }
};


TEST(libprocess, dispatch)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DispatchMockProcess process;

  EXPECT_CALL(process, func())
    .Times(AtMost(1));

  PID<DispatchMockProcess> pid = process::spawn(&process);

  ASSERT_FALSE(!pid);

  process::dispatch(pid, &DispatchMockProcess::func);

  process::post(pid, process::TERMINATE);
  process::wait(pid);
}


class InstallMockProcess : public Process<InstallMockProcess>
{
public:
  InstallMockProcess() { install("func", &InstallMockProcess::func); }
  MOCK_METHOD0(func, void());
  virtual void operator () () { serve(); }
};


TEST(libprocess, install)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  InstallMockProcess process;

  EXPECT_CALL(process, func())
    .Times(AtMost(1));

  PID<InstallMockProcess> pid = process::spawn(&process);

  ASSERT_FALSE(!pid);

  process::post(pid, "func");

  process::post(pid, process::TERMINATE);
  process::wait(pid);
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
  virtual void operator () () { serve(); }
};


TEST(libprocess, inheritance)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  DerivedMockProcess process;

  EXPECT_CALL(process, func())
    .Times(AtMost(1));

  EXPECT_CALL(process, foo())
    .Times(AtMost(1));

  PID<DerivedMockProcess> pid1 = process::spawn(&process);

  ASSERT_FALSE(!pid1);

  process::dispatch(pid1, &DerivedMockProcess::func);

  PID<BaseMockProcess> pid2(process);
  PID<BaseMockProcess> pid3 = pid1;

  ASSERT_EQ(pid2, pid3);

  process::dispatch(pid3, &BaseMockProcess::foo);

  process::post(pid1, process::TERMINATE);
  process::wait(pid1);
}


int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

  return RUN_ALL_TESTS();
}
