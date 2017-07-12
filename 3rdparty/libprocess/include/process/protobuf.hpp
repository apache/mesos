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

#include <google/protobuf/message.h>
#include <google/protobuf/repeated_field.h>

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
  message.SerializeToString(&data);
  post(to, message.GetTypeName(), data.data(), data.size());
}


inline void post(const process::UPID& from,
                 const process::UPID& to,
                 const google::protobuf::Message& message)
{
  std::string data;
  message.SerializeToString(&data);
  post(from, to, message.GetTypeName(), data.data(), data.size());
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
  std::vector<T> result;
  for (int i = 0; i < items.size(); i++) {
    result.push_back(items.Get(i));
  }

  return result;
}

} // namespace protobuf {
} // namespace google {


template <typename T>
class ProtobufProcess : public process::Process<T>
{
public:
  virtual ~ProtobufProcess() {}

protected:
  virtual void visit(const process::MessageEvent& event)
  {
    if (protobufHandlers.count(event.message.name) > 0) {
      from = event.message.from; // For 'reply'.
      protobufHandlers[event.message.name](
          event.message.from, event.message.body);
      from = process::UPID();
    } else {
      process::Process<T>::visit(event);
    }
  }

  void send(const process::UPID& to,
            const google::protobuf::Message& message)
  {
    std::string data;
    message.SerializeToString(&data);
    process::Process<T>::send(to, message.GetTypeName(),
                              data.data(), data.size());
  }

  using process::Process<T>::send;

  void reply(const google::protobuf::Message& message)
  {
    CHECK(from) << "Attempting to reply without a sender";
    std::string data;
    message.SerializeToString(&data);
    send(from, message);
  }

  // TODO(vinod): Use ENUM_PARAMS for the overloads.
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
            typename P1, typename P1C>
  void install(
      void (T::*method)(const process::UPID&, P1C),
      P1 (M::*param1)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&handler1<M, P1, P1C>,
                   t, method, param1,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C>
  void install(
      void (T::*method)(const process::UPID&, P1C, P2C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&handler2<M, P1, P1C, P2, P2C>,
                   t, method, p1, p2,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C>
  void install(
      void (T::*method)(const process::UPID&, P1C, P2C, P3C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&handler3<M, P1, P1C, P2, P2C, P3, P3C>,
                   t, method, p1, p2, p3,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C>
  void install(
      void (T::*method)(const process::UPID&, P1C, P2C, P3C, P4C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&handler4<M, P1, P1C, P2, P2C, P3, P3C, P4, P4C>,
                   t, method, p1, p2, p3, p4,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C>
  void install(
      void (T::*method)(const process::UPID&, P1C, P2C, P3C, P4C, P5C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&handler5<M, P1, P1C, P2, P2C, P3, P3C, P4, P4C, P5, P5C>,
                   t, method, p1, p2, p3, p4, p5,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C,
            typename P6, typename P6C>
  void install(
      void (T::*method)(const process::UPID&, P1C, P2C, P3C, P4C, P5C, P6C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const,
      P6 (M::*p6)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&handler6<M, P1, P1C, P2, P2C, P3, P3C,
                                P4, P4C, P5, P5C, P6, P6C>,
                   t, method, p1, p2, p3, p4, p5, p6,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C,
            typename P6, typename P6C,
            typename P7, typename P7C>
  void install(
      void (T::*method)(const process::UPID&, P1C, P2C, P3C,
                                              P4C, P5C, P6C, P7C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const,
      P6 (M::*p6)() const,
      P7 (M::*p7)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&handler7<M, P1, P1C, P2, P2C, P3, P3C,
                                P4, P4C, P5, P5C, P6, P6C, P7, P7C>,
                   t, method, p1, p2, p3, p4, p5, p6, p7,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C,
            typename P6, typename P6C,
            typename P7, typename P7C,
            typename P8, typename P8C>
  void install(
      void (T::*method)(const process::UPID&, P1C, P2C, P3C,
                                              P4C, P5C, P6C, P7C, P8C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const,
      P6 (M::*p6)() const,
      P7 (M::*p7)() const,
      P8 (M::*p8)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&handler8<M, P1, P1C, P2, P2C, P3, P3C,
                                P4, P4C, P5, P5C, P6, P6C,
                                P7, P7C, P8, P8C>,
                   t, method, p1, p2, p3, p4, p5, p6, p7, p8,
                   lambda::_1, lambda::_2);
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
            typename P1, typename P1C>
  void install(
      void (T::*method)(P1C),
      P1 (M::*param1)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&_handler1<M, P1, P1C>,
                   t, method, param1,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C>
  void install(
      void (T::*method)(P1C, P2C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&_handler2<M, P1, P1C, P2, P2C>,
                   t, method, p1, p2,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C>
  void install(
      void (T::*method)(P1C, P2C, P3C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&_handler3<M, P1, P1C, P2, P2C, P3, P3C>,
                   t, method, p1, p2, p3,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C>
  void install(
      void (T::*method)(P1C, P2C, P3C, P4C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&_handler4<M, P1, P1C, P2, P2C, P3, P3C, P4, P4C>,
                   t, method, p1, p2, p3, p4,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C>
  void install(
      void (T::*method)(P1C, P2C, P3C, P4C, P5C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&_handler5<M, P1, P1C, P2, P2C, P3, P3C, P4, P4C, P5, P5C>,
                   t, method, p1, p2, p3, p4, p5,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C,
            typename P6, typename P6C>
  void install(
      void (T::*method)(P1C, P2C, P3C, P4C, P5C, P6C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const,
      P6 (M::*p6)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&_handler6<M, P1, P1C, P2, P2C, P3, P3C,
                                 P4, P4C, P5, P5C, P6, P6C>,
                   t, method, p1, p2, p3, p4, p5, p6,
                   lambda::_1, lambda::_2);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C,
            typename P6, typename P6C,
            typename P7, typename P7C>
  void install(
      void (T::*method)(P1C, P2C, P3C, P4C, P5C, P6C, P7C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const,
      P6 (M::*p6)() const,
      P7 (M::*p7)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      lambda::bind(&_handler7<M, P1, P1C, P2, P2C, P3, P3C,
                                 P4, P4C, P5, P5C, P6, P6C, P7, P7C>,
                   t, method, p1, p2, p3, p4, p5, p6, p7,
                   lambda::_1, lambda::_2);
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
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(sender, m);
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
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
            typename P1, typename P1C>
  static void handler1(
      T* t,
      void (T::*method)(const process::UPID&, P1C),
      P1 (M::*p1)() const,
      const process::UPID& sender,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(sender, google::protobuf::convert((&m->*p1)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C>
  static void handler2(
      T* t,
      void (T::*method)(const process::UPID&, P1C, P2C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      const process::UPID& sender,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(sender,
                   google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C>
  static void handler3(
      T* t,
      void (T::*method)(const process::UPID&, P1C, P2C, P3C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      const process::UPID& sender,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(sender,
                   google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()),
                   google::protobuf::convert((&m->*p3)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C>
  static void handler4(
      T* t,
      void (T::*method)(const process::UPID&, P1C, P2C, P3C, P4C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      const process::UPID& sender,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(sender,
                   google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()),
                   google::protobuf::convert((&m->*p3)()),
                   google::protobuf::convert((&m->*p4)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C>
  static void handler5(
      T* t,
      void (T::*method)(const process::UPID&, P1C, P2C, P3C, P4C, P5C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const,
      const process::UPID& sender,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(sender,
                   google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()),
                   google::protobuf::convert((&m->*p3)()),
                   google::protobuf::convert((&m->*p4)()),
                   google::protobuf::convert((&m->*p5)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C,
            typename P6, typename P6C>
  static void handler6(
      T* t,
      void (T::*method)(const process::UPID&, P1C, P2C, P3C, P4C, P5C, P6C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const,
      P6 (M::*p6)() const,
      const process::UPID& sender,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(sender,
                   google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()),
                   google::protobuf::convert((&m->*p3)()),
                   google::protobuf::convert((&m->*p4)()),
                   google::protobuf::convert((&m->*p5)()),
                   google::protobuf::convert((&m->*p6)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C,
            typename P6, typename P6C,
            typename P7, typename P7C>
  static void handler7(
      T* t,
      void (T::*method)(
          const process::UPID&, P1C, P2C, P3C, P4C, P5C, P6C, P7C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const,
      P6 (M::*p6)() const,
      P7 (M::*p7)() const,
      const process::UPID& sender,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(sender,
                   google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()),
                   google::protobuf::convert((&m->*p3)()),
                   google::protobuf::convert((&m->*p4)()),
                   google::protobuf::convert((&m->*p5)()),
                   google::protobuf::convert((&m->*p6)()),
                   google::protobuf::convert((&m->*p7)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C,
            typename P6, typename P6C,
            typename P7, typename P7C,
            typename P8, typename P8C>
  static void handler8(
      T* t,
      void (T::*method)(
          const process::UPID&, P1C, P2C, P3C, P4C, P5C, P6C, P7C, P8C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const,
      P6 (M::*p6)() const,
      P7 (M::*p7)() const,
      P8 (M::*p8)() const,
      const process::UPID& sender,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(sender,
                   google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()),
                   google::protobuf::convert((&m->*p3)()),
                   google::protobuf::convert((&m->*p4)()),
                   google::protobuf::convert((&m->*p5)()),
                   google::protobuf::convert((&m->*p6)()),
                   google::protobuf::convert((&m->*p7)()),
                   google::protobuf::convert((&m->*p8)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  // Handlers that ignore the sender.
  template <typename M>
  static void _handlerM(
      T* t,
      void (T::*method)(const M&),
      const process::UPID&,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(m);
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
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
            typename P1, typename P1C>
  static void _handler1(
      T* t,
      void (T::*method)(P1C),
      P1 (M::*p1)() const,
      const process::UPID&,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(google::protobuf::convert((&m->*p1)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C>
  static void _handler2(
      T* t,
      void (T::*method)(P1C, P2C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      const process::UPID&,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C>
  static void _handler3(
      T* t,
      void (T::*method)(P1C, P2C, P3C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      const process::UPID&,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()),
                   google::protobuf::convert((&m->*p3)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C>
  static void _handler4(
      T* t,
      void (T::*method)(P1C, P2C, P3C, P4C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      const process::UPID&,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()),
                   google::protobuf::convert((&m->*p3)()),
                   google::protobuf::convert((&m->*p4)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C>
  static void _handler5(
      T* t,
      void (T::*method)(P1C, P2C, P3C, P4C, P5C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const,
      const process::UPID&,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()),
                   google::protobuf::convert((&m->*p3)()),
                   google::protobuf::convert((&m->*p4)()),
                   google::protobuf::convert((&m->*p5)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C,
            typename P6, typename P6C>
  static void _handler6(
      T* t,
      void (T::*method)(P1C, P2C, P3C, P4C, P5C, P6C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const,
      P6 (M::*p6)() const,
      const process::UPID&,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()),
                   google::protobuf::convert((&m->*p3)()),
                   google::protobuf::convert((&m->*p4)()),
                   google::protobuf::convert((&m->*p5)()),
                   google::protobuf::convert((&m->*p6)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
    }
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C,
            typename P6, typename P6C,
            typename P7, typename P7C>
  static void _handler7(
      T* t,
      void (T::*method)(P1C, P2C, P3C, P4C, P5C, P6C, P7C),
      P1 (M::*p1)() const,
      P2 (M::*p2)() const,
      P3 (M::*p3)() const,
      P4 (M::*p4)() const,
      P5 (M::*p5)() const,
      P6 (M::*p6)() const,
      P7 (M::*p7)() const,
      const process::UPID&,
      const std::string& data)
  {
    M m;
    m.ParseFromString(data);
    if (m.IsInitialized()) {
      (t->*method)(google::protobuf::convert((&m->*p1)()),
                   google::protobuf::convert((&m->*p2)()),
                   google::protobuf::convert((&m->*p3)()),
                   google::protobuf::convert((&m->*p4)()),
                   google::protobuf::convert((&m->*p5)()),
                   google::protobuf::convert((&m->*p6)()),
                   google::protobuf::convert((&m->*p7)()));
    } else {
      LOG(WARNING) << "Initialization errors: "
                   << m.InitializationErrorString();
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

  virtual ~ReqResProcess()
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
