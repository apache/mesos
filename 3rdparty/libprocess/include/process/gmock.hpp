#ifndef __PROCESS_GMOCK_HPP__
#define __PROCESS_GMOCK_HPP__

#include <pthread.h>

#include <gmock/gmock.h>

#include <process/dispatch.hpp>
#include <process/event.hpp>
#include <process/filter.hpp>
#include <process/pid.hpp>

#include <stout/exit.hpp>
#include <stout/nothing.hpp>

// NOTE: The gmock library relies on std::tr1::tuple. The gmock
// library provides multiple possible 'tuple' implementations but it
// still uses std::tr1::tuple as the "type" name, hence our use of it
// in this file.


#define FUTURE_MESSAGE(name, from, to)          \
  process::FutureMessage(name, from, to)

#define DROP_MESSAGE(name, from, to)            \
  process::FutureMessage(name, from, to, true)

// The mechanism of how we match method dispatches is done by
// comparing std::type_info of the member function pointers. Because
// of this, the method function pointer passed to either
// FUTURE_DISPATCH or DROP_DISPATCH must match exactly the member
// function that is passed to the dispatch method.
// TODO(tnachen): In a situation where a base class has a virtual
// function and that a derived class overrides, and if in unit tests
// we want to verify it calls the exact derived member function, we
// need to change how dispatch matching works. One possible way is to
// move the dispatch matching logic at event dequeue time, as we then
// have the actual Process the dispatch event is calling to.
#define FUTURE_DISPATCH(pid, method)            \
  process::FutureDispatch(pid, method)

#define DROP_DISPATCH(pid, method)              \
  process::FutureDispatch(pid, method, true)

#define DROP_MESSAGES(name, from, to)           \
  process::DropMessages(name, from, to)

#define EXPECT_NO_FUTURE_MESSAGES(name, from, to)       \
  process::ExpectNoFutureMessages(name, from, to)

#define DROP_DISPATCHES(pid, method)            \
  process::DropDispatches(pid, method)


ACTION_TEMPLATE(PromiseArg,
                HAS_1_TEMPLATE_PARAMS(int, k),
                AND_1_VALUE_PARAMS(promise))
{
  // TODO(benh): Use a shared_ptr for promise to defend against this
  // action getting invoked more than once (e.g., used via
  // WillRepeatedly). We won't be able to set it a second time but at
  // least we won't get a segmentation fault. We could also consider
  // warning users if they attempted to set it more than once.
  promise->set(std::tr1::get<k>(args));
  delete promise;
}


template <int index, typename T>
PromiseArgActionP<index, process::Promise<T>*> FutureArg(
    process::Future<T>* future)
{
  process::Promise<T>* promise = new process::Promise<T>();
  *future = promise->future();
  return PromiseArg<index>(promise);
}


ACTION_TEMPLATE(PromiseArgField,
                HAS_1_TEMPLATE_PARAMS(int, k),
                AND_2_VALUE_PARAMS(field, promise))
{
  // TODO(benh): Use a shared_ptr for promise to defend against this
  // action getting invoked more than once (e.g., used via
  // WillRepeatedly). We won't be able to set it a second time but at
  // least we won't get a segmentation fault. We could also consider
  // warning users if they attempted to set it more than once.
  promise->set(*(std::tr1::get<k>(args).*field));
  delete promise;
}


template <int index, typename Field, typename T>
PromiseArgFieldActionP2<index, Field, process::Promise<T>*> FutureArgField(
    Field field,
    process::Future<T>* future)
{
  process::Promise<T>* promise = new process::Promise<T>();
  *future = promise->future();
  return PromiseArgField<index>(field, promise);
}


ACTION_P2(PromiseSatisfy, promise, value)
{
  promise->set(value);
  delete promise;
}


template <typename T>
PromiseSatisfyActionP2<process::Promise<T>*, T> FutureSatisfy(
    process::Future<T>* future,
    T t)
{
  process::Promise<T>* promise = new process::Promise<T>();
  *future = promise->future();
  return PromiseSatisfy(promise, t);
}


inline PromiseSatisfyActionP2<process::Promise<Nothing>*, Nothing>
FutureSatisfy(process::Future<Nothing>* future)
{
  process::Promise<Nothing>* promise = new process::Promise<Nothing>();
  *future = promise->future();
  return PromiseSatisfy(promise, Nothing());
}


// This action invokes an "inner" action but captures the result and
// stores a copy of it in a future. Note that this is implemented
// similarly to the IgnoreResult action, which relies on the cast
// operator to Action<F> which must occur (implicitly) before the
// expression has completed, hence we can pass the Future<R>* all the
// way through to our action "implementation".
template <typename R, typename A>
class FutureResultAction
{
public:
  explicit FutureResultAction(process::Future<R>* future, const A& action)
    : future(future),
      action(action) {}

  template <typename F>
  operator ::testing::Action<F>() const
  {
    return ::testing::Action<F>(new Implementation<F>(future, action));
  }

private:
  template <typename F>
  class Implementation : public ::testing::ActionInterface<F>
  {
  public:
    explicit Implementation(process::Future<R>* future, const A& action)
      : action(action)
    {
      *future = promise.future();
    }

    virtual typename ::testing::ActionInterface<F>::Result Perform(
        const typename ::testing::ActionInterface<F>::ArgumentTuple& args)
    {
      const typename ::testing::ActionInterface<F>::Result result =
        action.Perform(args);
      promise.set(result);
      return result;
    }

  private:
    // Not copyable, not assignable.
    Implementation(const Implementation&);
    Implementation& operator = (const Implementation&);

    process::Promise<R> promise;
    const ::testing::Action<F> action;
  };

  process::Future<R>* future;
  const A action;
};


template <typename R, typename A>
FutureResultAction<R, A> FutureResult(
    process::Future<R>* future,
    const A& action)
{
  return FutureResultAction<R, A>(future, action);
}


namespace process {

class MockFilter : public Filter
{
public:
  MockFilter()
  {
    EXPECT_CALL(*this, filter(testing::A<const MessageEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(testing::A<const DispatchEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(testing::A<const HttpEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(testing::A<const ExitedEvent&>()))
      .WillRepeatedly(testing::Return(false));
  }

  MOCK_METHOD1(filter, bool(const MessageEvent&));
  MOCK_METHOD1(filter, bool(const DispatchEvent&));
  MOCK_METHOD1(filter, bool(const HttpEvent&));
  MOCK_METHOD1(filter, bool(const ExitedEvent&));
};


// A definition of a libprocess filter to enable waiting for events
// (such as messages or dispatches) via in tests. This is not meant to
// be used directly by tests; tests should use macros like
// FUTURE_MESSAGE and FUTURE_DISPATCH instead.
class TestsFilter : public Filter
{
public:
  TestsFilter()
  {
    // We use a recursive mutex here in the event that satisfying the
    // future created in FutureMessage or FutureDispatch via the
    // FutureArgField or FutureSatisfy actions invokes callbacks (from
    // Future::then or Future::onAny, etc) that themselves invoke
    // FutureDispatch or FutureMessage.
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex, &attr);
    pthread_mutexattr_destroy(&attr);
  }

  virtual bool filter(const MessageEvent& event) { return handle(event); }
  virtual bool filter(const DispatchEvent& event) { return handle(event); }
  virtual bool filter(const HttpEvent& event) { return handle(event); }
  virtual bool filter(const ExitedEvent& event) { return handle(event); }

  template <typename T>
  bool handle(const T& t)
  {
    pthread_mutex_lock(&mutex);
    bool drop = mock.filter(t);
    pthread_mutex_unlock(&mutex);
    return drop;
  }

  MockFilter mock;
  pthread_mutex_t mutex;
};


class FilterTestEventListener : public ::testing::EmptyTestEventListener
{
public:
  // Returns the singleton instance of the listener.
  static FilterTestEventListener* instance()
  {
    static FilterTestEventListener* listener = new FilterTestEventListener();
    return listener;
  }

  // Installs and returns the filter, creating it if necessary.
  TestsFilter* install()
  {
    if (!started) {
      EXIT(1)
        << "To use FUTURE/DROP_MESSAGE/DISPATCH, etc. you need to do the "
        << "following before you invoke RUN_ALL_TESTS():\n\n"
        << "\t::testing::TestEventListeners& listeners =\n"
        << "\t  ::testing::UnitTest::GetInstance()->listeners();\n"
        << "\tlisteners.Append(process::FilterTestEventListener::instance());";
    }

    if (filter != NULL) {
      return filter;
    }

    filter = new TestsFilter();

    // Set the filter in libprocess.
    process::filter(filter);

    return filter;
  }

  virtual void OnTestProgramStart(const ::testing::UnitTest&)
  {
    started = true;
  }

  virtual void OnTestEnd(const ::testing::TestInfo&)
  {
    if (filter != NULL) {
      // Remove the filter in libprocess _before_ deleting.
      process::filter(NULL);
      delete filter;
      filter = NULL;
    }
  }

private:
  FilterTestEventListener() : filter(NULL), started(false) {}

  TestsFilter* filter;

  // Indicates if we got the OnTestProgramStart callback in order to
  // detect if we have been properly added as a listener.
  bool started;
};


MATCHER_P3(MessageMatcher, name, from, to, "")
{
  const MessageEvent& event = ::std::tr1::get<0>(arg);
  return (testing::Matcher<std::string>(name).Matches(event.message->name) &&
          testing::Matcher<UPID>(from).Matches(event.message->from) &&
          testing::Matcher<UPID>(to).Matches(event.message->to));
}


MATCHER_P2(DispatchMatcher, pid, method, "")
{
  const DispatchEvent& event = ::std::tr1::get<0>(arg);
  return (testing::Matcher<UPID>(pid).Matches(event.pid) &&
          event.functionType.isSome() &&
          *event.functionType.get() == typeid(method));
}


template <typename Name, typename From, typename To>
Future<Message> FutureMessage(Name name, From from, To to, bool drop = false)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  pthread_mutex_lock(&filter->mutex);
  Future<Message> future;
  EXPECT_CALL(filter->mock, filter(testing::A<const MessageEvent&>()))
    .With(MessageMatcher(name, from, to))
    .WillOnce(testing::DoAll(FutureArgField<0>(&MessageEvent::message, &future),
                             testing::Return(drop)))
    .RetiresOnSaturation(); // Don't impose any subsequent expectations.
  pthread_mutex_unlock(&filter->mutex);
  return future;
}


template <typename PID, typename Method>
Future<Nothing> FutureDispatch(PID pid, Method method, bool drop = false)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  pthread_mutex_lock(&filter->mutex);
  Future<Nothing> future;
  EXPECT_CALL(filter->mock, filter(testing::A<const DispatchEvent&>()))
    .With(DispatchMatcher(pid, method))
    .WillOnce(testing::DoAll(FutureSatisfy(&future),
                             testing::Return(drop)))
    .RetiresOnSaturation(); // Don't impose any subsequent expectations.
  pthread_mutex_unlock(&filter->mutex);
  return future;
}


template <typename Name, typename From, typename To>
void DropMessages(Name name, From from, To to)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  pthread_mutex_lock(&filter->mutex);
  EXPECT_CALL(filter->mock, filter(testing::A<const MessageEvent&>()))
    .With(MessageMatcher(name, from, to))
    .WillRepeatedly(testing::Return(true));
  pthread_mutex_unlock(&filter->mutex);
}


template <typename Name, typename From, typename To>
void ExpectNoFutureMessages(Name name, From from, To to)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  pthread_mutex_lock(&filter->mutex);
  EXPECT_CALL(filter->mock, filter(testing::A<const MessageEvent&>()))
    .With(MessageMatcher(name, from, to))
    .Times(0);
  pthread_mutex_unlock(&filter->mutex);
}


template <typename PID, typename Method>
void DropDispatches(PID pid, Method method)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  pthread_mutex_lock(&filter->mutex);
  EXPECT_CALL(filter->mock, filter(testing::A<const DispatchEvent&>()))
    .With(DispatchMatcher(pid, method))
    .WillRepeatedly(testing::Return(true));
  pthread_mutex_unlock(&filter->mutex);
}

} // namespace process {

#endif // __PROCESS_GMOCK_HPP__
