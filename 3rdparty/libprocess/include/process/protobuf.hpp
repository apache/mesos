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

#ifndef __PROCESS_PROTOBUF_HPP__
#define __PROCESS_PROTOBUF_HPP__

#include <glog/logging.h>

#include <google/protobuf/arena.h>
#include <google/protobuf/message.h>
#include <google/protobuf/repeated_field.h>

#include <iterator>
#include <set>
#include <vector>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/lambda.hpp>


// Provides an implementation of process::post that for a protobuf.
namespace process {

inline void post(const process::UPID& to,
                 const google::protobuf::Message& message)
{
  std::string data;
  if (message.SerializeToString(&data)) {
    post(to, message.GetTypeName(), data.data(), data.size());
  } else {
    LOG(ERROR) << "Failed to post '" << message.GetTypeName() << "' to "
               << to << ": Failed to serialize";
  }
}


inline void post(const process::UPID& from,
                 const process::UPID& to,
                 const google::protobuf::Message& message)
{
  std::string data;
  if (message.SerializeToString(&data)) {
    post(from, to, message.GetTypeName(), data.data(), data.size());
  } else {
    LOG(ERROR) << "Failed to post '" << message.GetTypeName() << "' to "
               << to << ": Failed to serialize";
  }
}

} // namespace process {


// The rest of this file provides libprocess "support" for using
// protocol buffers. In particular, this file defines a subclass of
// Process (ProtobufProcess) that allows you to install protocol
// buffer handlers in addition to normal message and HTTP
// handlers. Install handlers can optionally take the sender's UPID
// as their first argument.
// Note that this header file assumes you will be linking
// against BOTH libprotobuf and libglog.

namespace google {
namespace protobuf {

// Type conversions helpful for changing between protocol buffer types
// and standard C++ types (for parameters).
template <typename T>
const T& convert(const T& t)
{
  return t;
}


template <typename T>
std::vector<T> convert(const google::protobuf::RepeatedPtrField<T>& items)
{
  return std::vector<T>(items.begin(), items.end());
}


template <typename T>
std::vector<T> convert(google::protobuf::RepeatedPtrField<T>&& items)
{
  return std::vector<T>(
      std::make_move_iterator(items.begin()),
      std::make_move_iterator(items.end()));
}

} // namespace protobuf {
} // namespace google {


template <typename T>
class ProtobufProcess : public process::Process<T>
{
public:
  ~ProtobufProcess() override {}

protected:
  void consume(process::MessageEvent&& event) override
  {
    if (protobufHandlers.count(event.message.name) > 0) {
      from = event.message.from; // For 'reply'.
      protobufHandlers[event.message.name](
          event.message.from, event.message.body);
      from = process::UPID();
    } else {
      process::Process<T>::consume(std::move(event));
    }
  }

  void send(const process::UPID& to,
            const google::protobuf::Message& message)
  {
    std::string data;
    if (message.SerializeToString(&data)) {
      process::Process<T>::send(to, message.GetTypeName(), std::move(data));
    } else {
      LOG(ERROR) << "Failed to send '" << message.GetTypeName() << "' to "
                 << to << ": Failed to serialize";
    }
  }

  using process::Process<T>::send;

  void reply(const google::protobuf::Message& message)
  {
    CHECK(from) << "Attempting to reply without a sender";
    send(from, message);
  }

  // Installs that take the sender as the first argument.
  template <typename M>
  void install(void (T::*method)(const process::UPID&, const M&))
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&handlerM<M>,
                   t, method,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M>
  void install(void (T::*method)(const process::UPID&, M&&))
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&handlerMutM<M>,
                   t, method,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M, typename P>
  using MessageProperty = P(M::*)() const;

  template <typename M>
  void install(void (T::*method)(const process::UPID&))
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&handler0,
                   t, method,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename ...P, typename ...PC>
  void install(
      void (T::*method)(const process::UPID&, PC...),
      MessageProperty<M, P>... param)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(static_cast<void(&)(
                       T*,
                       void (T::*)(const process::UPID&, PC...),
                       const process::UPID&,
                       const std::string&,
                       MessageProperty<M, P>...)>(handlerN),
                   t, method,
                   lambda::_1, lambda::_2, param...);
    delete m;
  }

  // Installs that do not take the sender.
  template <typename M>
  void install(void (T::*method)(const M&))
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&_handlerM<M>,
                   t, method,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M>
  void install(void (T::*method)(M&&))
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&_handlerMutM<M>,
                   t, method,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M>
  void install(void (T::*method)())
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&_handler0,
                   t, method,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename ...P, typename ...PC>
  void install(
      void (T::*method)(PC...),
      MessageProperty<M, P>... param)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(static_cast<void(&)(
                       T*,
                       void (T::*)(PC...),
                       const process::UPID&,
                       const std::string&,
                       MessageProperty<M, P>...)>(_handlerN),
                   t, method,
                   lambda::_1, lambda::_2, param...);
    delete m;
  }

  using process::Process<T>::install;

private:
  // Handlers that take the sender as the first argument.
  template <typename M>
  static void handlerM(
      T* t,
      void (T::*method)(const process::UPID&, const M&),
      const process::UPID& sender,
      const std::string& data)
  {
    google::protobuf::Arena arena;
    M* m = CHECK_NOTNULL(google::protobuf::Arena::CreateMessage<M>(&arena));

    if (m->ParseFromString(data)) {
      (t->*method)(sender, *m);
    } else {
      LOG(ERROR) << "Failed to deserialize '" << m->GetTypeName()
                 << "' from " << sender;
    }
  }

  template <typename M>
  static void handlerMutM(
      T* t,
      void (T::*method)(const process::UPID&, M&&),
      const process::UPID& sender,
      const std::string& data)
  {
    M m;

    if (m.ParseFromString(data)) {
      (t->*method)(sender, std::move(m));
    } else {
      LOG(ERROR) << "Failed to deserialize '" << m.GetTypeName()
                 << "' from " << sender;
    }
  }

  static void handler0(
      T* t,
      void (T::*method)(const process::UPID&),
      const process::UPID& sender,
      const std::string& data)
  {
    (t->*method)(sender);
  }

  template <typename M,
            typename ...P, typename ...PC>
  static void handlerN(
      T* t,
      void (T::*method)(const process::UPID&, PC...),
      const process::UPID& sender,
      const std::string& data,
      MessageProperty<M, P>... p)
  {
    google::protobuf::Arena arena;
    M* m = CHECK_NOTNULL(google::protobuf::Arena::CreateMessage<M>(&arena));

    if (m->ParseFromString(data)) {
      (t->*method)(sender, google::protobuf::convert((m->*p)())...);
    } else {
      LOG(ERROR) << "Failed to deserialize '" << m->GetTypeName()
                 << "' from " << sender;
    }
  }

  // Handlers that ignore the sender.
  template <typename M>
  static void _handlerM(
      T* t,
      void (T::*method)(const M&),
      const process::UPID& sender,
      const std::string& data)
  {
    google::protobuf::Arena arena;
    M* m = CHECK_NOTNULL(google::protobuf::Arena::CreateMessage<M>(&arena));

    if (m->ParseFromString(data)) {
      (t->*method)(*m);
    } else {
      LOG(ERROR) << "Failed to deserialize '" << m->GetTypeName()
                 << "' from " << sender;
    }
  }

  template <typename M>
  static void _handlerMutM(
      T* t,
      void (T::*method)(M&&),
      const process::UPID& sender,
      const std::string& data)
  {
    M m;

    if (m.ParseFromString(data)) {
      (t->*method)(std::move(m));
    } else {
      LOG(ERROR) << "Failed to deserialize '" << m.GetTypeName()
                 << "' from " << sender;
    }
  }

  static void _handler0(
      T* t,
      void (T::*method)(),
      const process::UPID&,
      const std::string& data)
  {
    (t->*method)();
  }

  template <typename M,
            typename ...P, typename ...PC>
  static void _handlerN(
      T* t,
      void (T::*method)(PC...),
      const process::UPID& sender,
      const std::string& data,
      MessageProperty<M, P>... p)
  {
    google::protobuf::Arena arena;
    M* m = CHECK_NOTNULL(google::protobuf::Arena::CreateMessage<M>(&arena));

    if (m->ParseFromString(data)) {
      (t->*method)(google::protobuf::convert((m->*p)())...);
    } else {
      LOG(ERROR) << "Failed to deserialize '" << m->GetTypeName()
                 << "' from " << sender;
    }
  }

  typedef lambda::function<
      void(const process::UPID&, const std::string&)> handler;
  hashmap<std::string, handler> protobufHandlers;

  // Sender of "current" message, inaccessible by subclasses.
  // This is only used for reply().
  process::UPID from;
};


// Implements a process for sending protobuf "requests" to a process
// and waiting for a protobuf "response", but uses futures so that
// this can be done without needing to implement a process.
template <typename Req, typename Res>
class ReqResProcess : public ProtobufProcess<ReqResProcess<Req, Res>>
{
public:
  ReqResProcess(const process::UPID& _pid, const Req& _req)
    : process::ProcessBase(process::ID::generate("__req_res__")),
      pid(_pid),
      req(_req)
  {
    ProtobufProcess<ReqResProcess<Req, Res>>::template
      install<Res>(&ReqResProcess<Req, Res>::response);
  }

  ~ReqResProcess() override
  {
    // Discard the promise.
    promise.discard();
  }

  process::Future<Res> run()
  {
    promise.future().onDiscard(defer(this, &ReqResProcess::discarded));

    ProtobufProcess<ReqResProcess<Req, Res>>::send(pid, req);

    return promise.future();
  }

private:
  void discarded()
  {
    promise.discard();
    process::terminate(this);
  }

  void response(const Res& res)
  {
    promise.set(res);
    process::terminate(this);
  }

  const process::UPID pid;
  const Req req;
  process::Promise<Res> promise;
};


// Allows you to describe request/response protocols and then use
// those for sending requests and getting back responses.
template <typename Req, typename Res>
struct Protocol
{
  process::Future<Res> operator()(
      const process::UPID& pid,
      const Req& req) const
  {
    // Help debugging by adding some "type constraints".
    { Req* req = nullptr; google::protobuf::Message* m = req; (void)m; }
    { Res* res = nullptr; google::protobuf::Message* m = res; (void)m; }

    ReqResProcess<Req, Res>* process = new ReqResProcess<Req, Res>(pid, req);
    process::spawn(process, true);
    return process::dispatch(process, &ReqResProcess<Req, Res>::run);
  }
};

#endif // __PROCESS_PROTOBUF_HPP__
