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

#ifndef __PROCESS_EVENT_HPP__
#define __PROCESS_EVENT_HPP__

#include <memory> // TODO(benh): Replace shared_ptr with unique_ptr.

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/message.hpp>
#include <process/socket.hpp>

#include <stout/abort.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>

namespace process {

// Forward declarations.
class ProcessBase;
struct MessageEvent;
struct DispatchEvent;
struct HttpEvent;
struct ExitedEvent;
struct TerminateEvent;


struct EventVisitor
{
  virtual ~EventVisitor() {}
  virtual void visit(const MessageEvent&) {}
  virtual void visit(const DispatchEvent&) {}
  virtual void visit(const HttpEvent&) {}
  virtual void visit(const ExitedEvent&) {}
  virtual void visit(const TerminateEvent&) {}
};


struct EventConsumer
{
  virtual ~EventConsumer() {}
  virtual void consume(MessageEvent&&) {}
  virtual void consume(DispatchEvent&&) {}
  virtual void consume(HttpEvent&&) {}
  virtual void consume(ExitedEvent&&) {}
  virtual void consume(TerminateEvent&&) {}
};


struct Event
{
  virtual ~Event() {}

  virtual void visit(EventVisitor* visitor) const = 0;
  virtual void consume(EventConsumer* consumer) && = 0;

  template <typename T>
  bool is() const
  {
    bool result = false;
    struct IsVisitor : EventVisitor
    {
      explicit IsVisitor(bool* _result) : result(_result) {}
      void visit(const T&) override { *result = true; }
      bool* result;
    } visitor(&result);
    visit(&visitor);
    return result;
  }

  template <typename T>
  const T& as() const
  {
    const T* result = nullptr;
    struct AsVisitor : EventVisitor
    {
      explicit AsVisitor(const T** _result) : result(_result) {}
      void visit(const T& t) override { *result = &t; }
      const T** result;
    } visitor(&result);
    visit(&visitor);
    if (result == nullptr) {
      ABORT("Attempting to \"cast\" event incorrectly!");
    }
    return *result;
  }

  // JSON representation for an Event.
  operator JSON::Object() const;
};


struct MessageEvent : Event
{
  explicit MessageEvent(Message&& _message)
    : message(std::move(_message)) {}

  MessageEvent(
      const UPID& from,
      const UPID& to,
      const std::string& name,
      const char* data,
      size_t length)
    : message{name, from, to, std::string(data, length)} {}

  MessageEvent(
      const UPID& from,
      const UPID& to,
      std::string&& name,
      std::string&& data)
    : message{std::move(name), from, to, std::move(data)} {}

  MessageEvent(MessageEvent&& that) = default;
  MessageEvent(const MessageEvent& that) = delete;
  MessageEvent& operator=(MessageEvent&&) = default;
  MessageEvent& operator=(const MessageEvent&) = delete;

  void visit(EventVisitor* visitor) const override
  {
    visitor->visit(*this);
  }

  void consume(EventConsumer* consumer) && override
  {
    consumer->consume(std::move(*this));
  }

  Message message;
};


struct HttpEvent : Event
{
  HttpEvent(
      std::unique_ptr<http::Request>&& _request,
      std::unique_ptr<Promise<http::Response>>&& _response)
    : request(std::move(_request)),
      response(std::move(_response)) {}

  HttpEvent(HttpEvent&&) = default;
  HttpEvent(const HttpEvent&) = delete;
  HttpEvent& operator=(HttpEvent&&) = default;
  HttpEvent& operator=(const HttpEvent&) = delete;

  ~HttpEvent() override
  {
    if (response) {
      // Fail the response in case it wasn't set.
      response->set(http::InternalServerError());
    }
  }

  void visit(EventVisitor* visitor) const override
  {
    visitor->visit(*this);
  }

  void consume(EventConsumer* consumer) && override
  {
    consumer->consume(std::move(*this));
  }

  std::unique_ptr<http::Request> request;
  std::unique_ptr<Promise<http::Response>> response;
};


struct DispatchEvent : Event
{
  DispatchEvent(
      std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> _f,
      const Option<const std::type_info*>& _functionType)
    : f(std::move(_f)),
      functionType(_functionType)
  {}

  DispatchEvent(DispatchEvent&&) = default;
  DispatchEvent(const DispatchEvent&) = delete;
  DispatchEvent& operator=(DispatchEvent&&) = default;
  DispatchEvent& operator=(const DispatchEvent&) = delete;

  void visit(EventVisitor* visitor) const override
  {
    visitor->visit(*this);
  }

  void consume(EventConsumer* consumer) && override
  {
    consumer->consume(std::move(*this));
  }

  // Function to get invoked as a result of this dispatch event.
  std::unique_ptr<lambda::CallableOnce<void(ProcessBase*)>> f;

  Option<const std::type_info*> functionType;
};


struct ExitedEvent : Event
{
  explicit ExitedEvent(const UPID& _pid)
    : pid(_pid) {}

  ExitedEvent(ExitedEvent&&) = default;
  ExitedEvent(const ExitedEvent&) = delete;
  ExitedEvent& operator=(ExitedEvent&&) = default;
  ExitedEvent& operator=(const ExitedEvent&) = delete;

  void visit(EventVisitor* visitor) const override
  {
    visitor->visit(*this);
  }

  void consume(EventConsumer* consumer) && override
  {
    consumer->consume(std::move(*this));
  }

  UPID pid;
};


struct TerminateEvent : Event
{
  TerminateEvent(const UPID& _from, bool _inject)
    : from(_from), inject(_inject) {}

  TerminateEvent(TerminateEvent&&) = default;
  TerminateEvent(const TerminateEvent&) = delete;
  TerminateEvent& operator=(TerminateEvent&&) = default;
  TerminateEvent& operator=(const TerminateEvent&) = delete;

  void visit(EventVisitor* visitor) const override
  {
    visitor->visit(*this);
  }

  void consume(EventConsumer* consumer) && override
  {
    consumer->consume(std::move(*this));
  }

  UPID from;
  bool inject;
};


inline Event::operator JSON::Object() const
{
  JSON::Object object;

  struct Visitor : EventVisitor
  {
    explicit Visitor(JSON::Object* _object) : object(_object) {}

    void visit(const MessageEvent& event) override
    {
      object->values["type"] = "MESSAGE";

      const Message& message = event.message;

      object->values["name"] = message.name;
      object->values["from"] = stringify(message.from);
      object->values["to"] = stringify(message.to);
      object->values["body"] = message.body;
    }

    void visit(const HttpEvent& event) override
    {
      object->values["type"] = "HTTP";

      const http::Request& request = *event.request;

      object->values["method"] = request.method;
      object->values["url"] = stringify(request.url);
    }

    void visit(const DispatchEvent& event) override
    {
      object->values["type"] = "DISPATCH";
    }

    void visit(const ExitedEvent& event) override
    {
      object->values["type"] = "EXITED";
    }

    void visit(const TerminateEvent& event) override
    {
      object->values["type"] = "TERMINATE";
    }

    JSON::Object* object;
  } visitor(&object);

  visit(&visitor);

  return object;
}

} // namespace process {

#endif // __PROCESS_EVENT_HPP__
