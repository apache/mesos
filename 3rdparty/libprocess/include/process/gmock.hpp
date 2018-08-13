// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_GMOCK_HPP__
#define __PROCESS_GMOCK_HPP__

#include <gmock/gmock.h>

#include <process/dispatch.hpp>
#include <process/event.hpp>
#include <process/filter.hpp>
#include <process/pid.hpp>

#include <stout/exit.hpp>
#include <stout/nothing.hpp>
#include <stout/synchronized.hpp>


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

#define EXPECT_NO_FUTURE_DISPATCHES(pid, method)        \
  process::ExpectNoFutureDispatches(pid, method)


#define FUTURE_EXITED(from, to)                 \
  process::FutureExited(from, to)

#define DROP_EXITED(from, to)                   \
  process::FutureExited(from, to, true)


ACTION_TEMPLATE(PromiseArg,
                HAS_1_TEMPLATE_PARAMS(int, k),
                AND_1_VALUE_PARAMS(promise))
{
  // TODO(benh): Use a shared_ptr for promise to defend against this
  // action getting invoked more than once (e.g., used via
  // WillRepeatedly). We won't be able to set it a second time but at
  // least we won't get a segmentation fault. We could also consider
  // warning users if they attempted to set it more than once.
  promise->set(std::get<k>(args));
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
  promise->set(*(std::get<k>(args).*field));
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


ACTION_TEMPLATE(PromiseArgNotPointerField,
                HAS_1_TEMPLATE_PARAMS(int, k),
                AND_2_VALUE_PARAMS(field, promise))
{
  // TODO(benh): Use a shared_ptr for promise to defend against this
  // action getting invoked more than once (e.g., used via
  // WillRepeatedly). We won't be able to set it a second time but at
  // least we won't get a segmentation fault. We could also consider
  // warning users if they attempted to set it more than once.
  promise->set(std::get<k>(args).*field);
  delete promise;
}


template <int index, typename Field, typename T>
PromiseArgNotPointerFieldActionP2<index, Field, process::Promise<T>*>
FutureArgNotPointerField(
    Field field,
    process::Future<T>* future)
{
  process::Promise<T>* promise = new process::Promise<T>();
  *future = promise->future();
  return PromiseArgNotPointerField<index>(field, promise);
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

    typename ::testing::ActionInterface<F>::Result Perform(
        const typename ::testing::ActionInterface<F>::ArgumentTuple& args)
        override
    {
      const typename ::testing::ActionInterface<F>::Result result =
        action.Perform(args);
      promise.set(result);
      return result;
    }

  private:
    // Not copyable, not assignable.
    Implementation(const Implementation&);
    Implementation& operator=(const Implementation&);

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
    EXPECT_CALL(*this, filter(
        testing::A<const UPID&>(),
        testing::A<const MessageEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(
        testing::A<const UPID&>(),
        testing::A<const DispatchEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(
        testing::A<const UPID&>(),
        testing::A<const HttpEvent&>()))
      .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*this, filter(
        testing::A<const UPID&>(),
        testing::A<const ExitedEvent&>()))
      .WillRepeatedly(testing::Return(false));
  }

  MOCK_METHOD2(filter, bool(const UPID& process, const MessageEvent&));
  MOCK_METHOD2(filter, bool(const UPID& process, const DispatchEvent&));
  MOCK_METHOD2(filter, bool(const UPID& process, const HttpEvent&));
  MOCK_METHOD2(filter, bool(const UPID& process, const ExitedEvent&));
};


// A definition of a libprocess filter to enable waiting for events
// (such as messages or dispatches) via in tests. This is not meant to
// be used directly by tests; tests should use macros like
// FUTURE_MESSAGE and FUTURE_DISPATCH instead.
class TestsFilter : public Filter
{
public:
  TestsFilter() = default;

  bool filter(const UPID& process, const MessageEvent& event) override
  {
    return handle(process, event);
  }
  bool filter(const UPID& process, const DispatchEvent& event) override
  {
    return handle(process, event);
  }
  bool filter(const UPID& process, const HttpEvent& event) override
  {
    return handle(process, event);
  }
  bool filter(const UPID& process, const ExitedEvent& event) override
  {
    return handle(process, event);
  }

  template <typename T>
  bool handle(const UPID& process, const T& t)
  {
    synchronized (mutex) {
      return mock.filter(process, t);
    }
  }

  MockFilter mock;

  // We use a recursive mutex here in the event that satisfying the
  // future created in FutureMessage or FutureDispatch via the
  // FutureArgField or FutureSatisfy actions invokes callbacks (from
  // Future::then or Future::onAny, etc) that themselves invoke
  // FutureDispatch or FutureMessage.
  std::recursive_mutex mutex;
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
      EXIT(EXIT_FAILURE)
        << "To use FUTURE/DROP_MESSAGE/DISPATCH, etc. you need to do the "
        << "following before you invoke RUN_ALL_TESTS():\n\n"
        << "\t::testing::TestEventListeners& listeners =\n"
        << "\t  ::testing::UnitTest::GetInstance()->listeners();\n"
        << "\tlisteners.Append(process::FilterTestEventListener::instance());";
    }

    if (filter != nullptr) {
      return filter;
    }

    filter = new TestsFilter();

    // Set the filter in libprocess.
    process::filter(filter);

    return filter;
  }

  void OnTestProgramStart(const ::testing::UnitTest&) override
  {
    started = true;
  }

  void OnTestEnd(const ::testing::TestInfo&) override
  {
    if (filter != nullptr) {
      // Remove the filter in libprocess _before_ deleting.
      process::filter(nullptr);
      delete filter;
      filter = nullptr;
    }
  }

private:
  FilterTestEventListener() : filter(nullptr), started(false) {}

  TestsFilter* filter;

  // Indicates if we got the OnTestProgramStart callback in order to
  // detect if we have been properly added as a listener.
  bool started;
};


MATCHER_P2(MessageMatcher, name, from, "")
{
  const MessageEvent& event = ::std::get<1>(arg);
  return (testing::Matcher<std::string>(name).Matches(event.message.name) &&
          testing::Matcher<UPID>(from).Matches(event.message.from));
}


// This matches protobuf messages that are described using the
// standard protocol buffer "union" trick, see:
// https://developers.google.com/protocol-buffers/docs/techniques#union.
MATCHER_P3(UnionMessageMatcher, message, unionType, from, "")
{
  const process::MessageEvent& event = ::std::get<1>(arg);
  message_type message;

  return (testing::Matcher<std::string>(message.GetTypeName()).Matches(
              event.message.name) &&
          message.ParseFromString(event.message.body) &&
          testing::Matcher<unionType_type>(unionType).Matches(message.type()) &&
          testing::Matcher<process::UPID>(from).Matches(event.message.from));
}


MATCHER_P(DispatchMatcher, method, "")
{
  const DispatchEvent& event = ::std::get<1>(arg);
  return (event.functionType.isSome() &&
          *event.functionType.get() == typeid(method));
}


MATCHER_P(ExitedMatcher, from, "")
{
  const ExitedEvent& event = ::std::get<1>(arg);
  return testing::Matcher<process::UPID>(from).Matches(event.pid);
}


MATCHER_P3(HttpMatcher, message, path, deserializer, "")
{
  const HttpEvent& event = ::std::get<1>(arg);

  Try<message_type> message_ = deserializer(event.request->body);
  if (message_.isError()) {
    return false;
  }

  return (testing::Matcher<std::string>(path).Matches(event.request->url.path));
}


// See `UnionMessageMatcher` for more details on protobuf messages using the
// "union" trick.
MATCHER_P4(UnionHttpMatcher, message, unionType, path, deserializer, "")
{
  const HttpEvent& event = ::std::get<1>(arg);

  Try<message_type> message_ = deserializer(event.request->body);
  if (message_.isError()) {
    return false;
  }

  return (
      testing::Matcher<unionType_type>(unionType).Matches(message_->type()) &&
      testing::Matcher<std::string>(path).Matches(event.request->url.path));
}


template <typename Message, typename Path, typename Deserializer>
Future<http::Request> FutureHttpRequest(
    Message message,
    Path path,
    Deserializer deserializer,
    bool drop = false)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  Future<http::Request> future;
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(
        testing::A<const UPID&>(),
        testing::A<const HttpEvent&>()))
      .With(HttpMatcher(message, path, deserializer))
      .WillOnce(testing::DoAll(FutureArgField<1>(
                                   &HttpEvent::request,
                                   &future),
                               testing::Return(drop)))
      .RetiresOnSaturation(); // Don't impose any subsequent expectations.
  }

  return future;
}


template <typename Message,
          typename UnionType,
          typename Path,
          typename Deserializer>
Future<http::Request> FutureUnionHttpRequest(
    Message message,
    UnionType unionType,
    Path path,
    Deserializer deserializer,
    bool drop = false)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  Future<http::Request> future;
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(
        testing::A<const UPID&>(),
        testing::A<const HttpEvent&>()))
      .With(UnionHttpMatcher(message, unionType, path, deserializer))
      .WillOnce(testing::DoAll(FutureArgField<1>(
                                   &HttpEvent::request,
                                   &future),
                               testing::Return(drop)))
      .RetiresOnSaturation(); // Don't impose any subsequent expectations.
  }

  return future;
}


template <typename Name, typename From, typename To>
Future<Message> FutureMessage(Name name, From from, To to, bool drop = false)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  Future<Message> future;
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(to, testing::A<const MessageEvent&>()))
      .With(MessageMatcher(name, from))
      .WillOnce(testing::DoAll(FutureArgNotPointerField<1>(
                                   &MessageEvent::message,
                                   &future),
                               testing::Return(drop)))
      .RetiresOnSaturation(); // Don't impose any subsequent expectations.
  }

  return future;
}


template <typename Message, typename UnionType, typename From, typename To>
Future<process::Message> FutureUnionMessage(
    Message message, UnionType unionType, From from, To to, bool drop = false)
{
  TestsFilter* filter =
    FilterTestEventListener::instance()->install();

  Future<process::Message> future;
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(to, testing::A<const MessageEvent&>()))
      .With(UnionMessageMatcher(message, unionType, from))
      .WillOnce(testing::DoAll(FutureArgNotPointerField<1>(
                                   &MessageEvent::message,
                                   &future),
                               testing::Return(drop)))
      .RetiresOnSaturation(); // Don't impose any subsequent expectations.
  }

  return future;
}


template <typename PID, typename Method>
Future<Nothing> FutureDispatch(PID pid, Method method, bool drop = false)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  Future<Nothing> future;
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(pid, testing::A<const DispatchEvent&>()))
      .With(DispatchMatcher(method))
      .WillOnce(testing::DoAll(FutureSatisfy(&future),
                              testing::Return(drop)))
      .RetiresOnSaturation(); // Don't impose any subsequent expectations.
  }

  return future;
}


template <typename Name, typename From, typename To>
void DropMessages(Name name, From from, To to)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(to, testing::A<const MessageEvent&>()))
      .With(MessageMatcher(name, from))
      .WillRepeatedly(testing::Return(true));
  }
}


template <typename Message, typename UnionType, typename From, typename To>
void DropUnionMessages(Message message, UnionType unionType, From from, To to)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(to, testing::A<const MessageEvent&>()))
      .With(UnionMessageMatcher(message, unionType, from))
      .WillRepeatedly(testing::Return(true));
  }
}


template <typename Message, typename Path, typename Deserializer>
void DropHttpRequests(
    Message message,
    Path path,
    Deserializer deserializer,
    bool drop = false)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(
        testing::A<const UPID&>(),
        testing::A<const HttpEvent&>()))
      .With(HttpMatcher(message, path, deserializer))
      .WillRepeatedly(testing::Return(true));
  }
}


template <typename Message,
          typename UnionType,
          typename Path,
          typename Deserializer>
void DropUnionHttpRequests(
    Message message,
    UnionType unionType,
    Path path,
    Deserializer deserializer,
    bool drop = false)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  Future<http::Request> future;
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(
        testing::A<const UPID&>(),
        testing::A<const HttpEvent&>()))
      .With(UnionHttpMatcher(message, unionType, path, deserializer))
      .WillRepeatedly(testing::Return(true));
  }
}


template <typename Message, typename Path, typename Deserializer>
void ExpectNoFutureHttpRequests(
    Message message,
    Path path,
    Deserializer deserializer,
    bool drop = false)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(
        testing::A<const UPID&>(),
        testing::A<const HttpEvent&>()))
      .With(HttpMatcher(message, path, deserializer))
      .Times(0);
  }
}


template <typename Message,
          typename UnionType,
          typename Path,
          typename Deserializer>
void ExpectNoFutureUnionHttpRequests(
    Message message,
    UnionType unionType,
    Path path,
    Deserializer deserializer,
    bool drop = false)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  Future<http::Request> future;
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(
        testing::A<const UPID&>(),
        testing::A<const HttpEvent&>()))
      .With(UnionHttpMatcher(message, unionType, path, deserializer))
      .Times(0);
  }
}


template <typename Name, typename From, typename To>
void ExpectNoFutureMessages(Name name, From from, To to)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(to, testing::A<const MessageEvent&>()))
      .With(MessageMatcher(name, from))
      .Times(0);
  }
}


template <typename Message, typename UnionType, typename From, typename To>
void ExpectNoFutureUnionMessages(
    Message message, UnionType unionType, From from, To to)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(to, testing::A<const MessageEvent&>()))
      .With(UnionMessageMatcher(message, unionType, from))
      .Times(0);
  }
}


template <typename PID, typename Method>
void DropDispatches(PID pid, Method method)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(pid, testing::A<const DispatchEvent&>()))
      .With(DispatchMatcher(method))
      .WillRepeatedly(testing::Return(true));
  }
}


template <typename PID, typename Method>
void ExpectNoFutureDispatches(PID pid, Method method)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(pid, testing::A<const DispatchEvent&>()))
      .With(DispatchMatcher(method))
      .Times(0);
  }
}


template <typename From, typename To>
Future<Nothing> FutureExited(From from, To to, bool drop = false)
{
  TestsFilter* filter = FilterTestEventListener::instance()->install();
  Future<Nothing> future;
  synchronized (filter->mutex) {
    EXPECT_CALL(filter->mock, filter(to, testing::A<const ExitedEvent&>()))
      .With(ExitedMatcher(from))
      .WillOnce(testing::DoAll(FutureSatisfy(&future),
                              testing::Return(drop)))
      .RetiresOnSaturation(); // Don't impose any subsequent expectations.
  }

  return future;
}

} // namespace process {

#endif // __PROCESS_GMOCK_HPP__
