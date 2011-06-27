#ifndef __PROCESS_PROTOBUF_HPP__
#define __PROCESS_PROTOBUF_HPP__

#include <glog/logging.h>

#include <google/protobuf/message.h>
#include <google/protobuf/repeated_field.h>

#include <vector>

#include <tr1/unordered_map>

#include <process/process.hpp>


// Provides a "protocol buffer process", which is to say a subclass of
// Process that allows you to install protocol buffer handlers in
// addition to normal message and HTTP handlers. Then you can simply
// send around protocol buffer objects which will get passed to the
// appropriate handlers. Note that this header file assumes you will
// be linking against BOTH libprotobuf and libglog.


namespace google { namespace protobuf {

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

}} // namespace google { namespace protobuf {


template <typename T>
class ProtobufProcess : public process::Process<T>
{
public:
  ProtobufProcess(const std::string& id = "")
    : process::Process<T>(id) {}

  virtual ~ProtobufProcess() {}

protected:
  virtual void operator () ()
  {
    // TODO(benh): Shouldn't we just make Process::serve be a virtual
    // function, and then the one we get from process::Process will be
    // sufficient?
    do { if (serve() == process::TERMINATE) break; } while (true);
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

  const std::string& serve(double secs = 0, bool once = false)
  {
    do {
      const std::string& name = process::Process<T>::serve(secs, once);
      if (protobufHandlers.count(name) > 0) {
        protobufHandlers[name](process::Process<T>::body());
      } else {
        return name;
      }
    } while (!once);
  }

  template <typename M>
  void installProtobufHandler(void (T::*method)())
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      std::tr1::bind(&ProtobufProcess<T>::handler0,
                     t, method,
                     std::tr1::placeholders::_1);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C>
  void installProtobufHandler(void (T::*method)(P1C),
                              P1 (M::*param1)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      std::tr1::bind(&handler1<M, P1, P1C>,
                     t, method, param1,
                     std::tr1::placeholders::_1);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C>
  void installProtobufHandler(void (T::*method)(P1C, P2C),
                              P1 (M::*p1)() const,
                              P2 (M::*p2)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      std::tr1::bind(&handler2<M, P1, P1C, P2, P2C>,
                     t, method, p1, p2,
                     std::tr1::placeholders::_1);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C>
  void installProtobufHandler(void (T::*method)(P1C, P2C, P3C),
                              P1 (M::*p1)() const,
                              P2 (M::*p2)() const,
                              P3 (M::*p3)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      std::tr1::bind(&handler3<M, P1, P1C, P2, P2C, P3, P3C>,
                     t, method, p1, p2, p3,
                     std::tr1::placeholders::_1);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C>
  void installProtobufHandler(void (T::*method)(P1C, P2C, P3C, P4C),
                              P1 (M::*p1)() const,
                              P2 (M::*p2)() const,
                              P3 (M::*p3)() const,
                              P4 (M::*p4)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      std::tr1::bind(&handler4<M, P1, P1C, P2, P2C, P3, P3C, P4, P4C>,
                     t, method, p1, p2, p3, p4,
                     std::tr1::placeholders::_1);
    delete m;
  }

  template <typename M,
            typename P1, typename P1C,
            typename P2, typename P2C,
            typename P3, typename P3C,
            typename P4, typename P4C,
            typename P5, typename P5C>
  void installProtobufHandler(void (T::*method)(P1C, P2C, P3C, P4C, P5C),
                              P1 (M::*p1)() const,
                              P2 (M::*p2)() const,
                              P3 (M::*p3)() const,
                              P4 (M::*p4)() const,
                              P5 (M::*p5)() const)
  {
    google::protobuf::Message* m = new M();
    T* t = static_cast<T*>(this);
    protobufHandlers[m->GetTypeName()] =
      std::tr1::bind(&handler5<M, P1, P1C, P2, P2C, P3, P3C, P4, P4C, P5, P5C>,
                     t, method, p1, p2, p3, p4, p5,
                     std::tr1::placeholders::_1);
    delete m;
  }

private:
  static void handler0(T* t, void (T::*method)(),
                       const std::string& data)
  {
    (t->*method)();
  }

  template <typename M,
            typename P1, typename P1C>
  static void handler1(T* t, void (T::*method)(P1C),
                       P1 (M::*p1)() const,
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
  static void handler2(T* t, void (T::*method)(P1C, P2C),
                       P1 (M::*p1)() const,
                       P2 (M::*p2)() const,
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
  static void handler3(T* t, void (T::*method)(P1C, P2C, P3C),
                       P1 (M::*p1)() const,
                       P2 (M::*p2)() const,
                       P3 (M::*p3)() const,
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
  static void handler4(T* t, void (T::*method)(P1C, P2C, P3C, P4C),
                       P1 (M::*p1)() const,
                       P2 (M::*p2)() const,
                       P3 (M::*p3)() const,
                       P4 (M::*p4)() const,
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
  static void handler5(T* t, void (T::*method)(P1C, P2C, P3C, P4C, P5C),
                       P1 (M::*p1)() const,
                       P2 (M::*p2)() const,
                       P3 (M::*p3)() const,
                       P4 (M::*p4)() const,
                       P5 (M::*p5)() const,
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

  typedef std::tr1::function<void(const std::string&)> handler;
  std::tr1::unordered_map<std::string, handler> protobufHandlers;
};


namespace process {

inline void post(const process::UPID& to,
                 const google::protobuf::Message& message)
{
  std::string data;
  message.SerializeToString(&data);
  post(to, message.GetTypeName(), data.data(), data.size());
}


// template <typename Request, typename Response>
// class RequestResponseProcess
//   : public ProtobufProcess<RequestResponseProcess<Request, Response> >
// {
// public:
//   RequestResponseProcess(
//       const UPID& _pid,
//       const Request& _request,
//       const Promise<Response>& _promise)
//     : pid(_pid), request(_request), promise(_promise) {}

// protected:
//   virtual void operator () ()
//   {
//     send(pid, request);
//     ProcessBase::receive();
//     Response response;
//     CHECK(ProcessBase::name() == response.GetTypeName());
//     response.ParseFromString(ProcessBase::body());
//     promise.set(response);
//   }

// private:
//   const UPID pid;
//   const Request request;
//   Promise<Response> promise;
// };


// template <typename Request, typename Response>
// struct Protocol
// {
//   Future<Response> operator () (const UPID& pid, const Request& request)
//   {
//     Future<Response> future;
//     Promise<Response> promise;
//     promise.associate(future);
//     RequestResponseProcess<Request, Response>* process =
//       new RequestResponseProcess<Request, Response>(pid, request, promise);
//     spawn(process, true);
//     return future;
//   }
// };

} // namespace process {

#endif // __PROCESS_PROTOBUF_HPP__
