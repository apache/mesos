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


struct Event
{
  virtual ~Event() {}

  virtual void visit(EventVisitor* visitor) const = 0;

  template <typename T>
  bool is() const
  {
    bool result = false;
    struct IsVisitor : EventVisitor
    {
      explicit IsVisitor(bool* _result) : result(_result) {}
      virtual void visit(const T&) { *result = true; }
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
      virtual void visit(const T& t) { *result = &t; }
      const T** result;
    } visitor(&result);
    visit(&visitor);
    if (result == nullptr) {
      ABORT("Attempting to \"cast\" event incorrectly!");
    }
    return *result;
  }
};


struct MessageEvent : Event
{
  explicit MessageEvent(Message* _message)
    : message(_message) {}

  MessageEvent(const MessageEvent& that)
    : message(that.message == nullptr ? nullptr : new Message(*that.message)) {}

  virtual ~MessageEvent()
  {
    delete message;
  }

  virtual void visit(EventVisitor* visitor) const
  {
    visitor->visit(*this);
  }

  Message* const message;

private:
  // Keep MessageEvent not assignable even though we made it
  // copyable.
  // Note that we are violating the "rule of three" here but it helps
  // keep the fields const.
  MessageEvent& operator=(const MessageEvent&);
};


struct HttpEvent : Event
{
  HttpEvent(
      http::Request* _request,
      Promise<http::Response>* _response)
    : request(_request),
      response(_response) {}

  virtual ~HttpEvent()
  {
    delete request;

    // Fail the response in case it wasn't set.
    response->set(http::InternalServerError());
    delete response;
  }

  virtual void visit(EventVisitor* visitor) const
  {
    visitor->visit(*this);
  }

  http::Request* const request;
  Promise<http::Response>* response;

private:
  // Not copyable, not assignable.
  HttpEvent(const HttpEvent&);
  HttpEvent& operator=(const HttpEvent&);
};


struct DispatchEvent : Event
{
  DispatchEvent(
      const UPID& _pid,
      const std::shared_ptr<lambda::function<void(ProcessBase*)>>& _f,
      const Option<const std::type_info*>& _functionType)
    : pid(_pid),
      f(_f),
      functionType(_functionType)
  {}

  virtual void visit(EventVisitor* visitor) const
  {
    visitor->visit(*this);
  }

  // PID receiving the dispatch.
  const UPID pid;

  // Function to get invoked as a result of this dispatch event.
  const std::shared_ptr<lambda::function<void(ProcessBase*)>> f;

  const Option<const std::type_info*> functionType;

private:
  // Not copyable, not assignable.
  DispatchEvent(const DispatchEvent&);
  DispatchEvent& operator=(const DispatchEvent&);
};


struct ExitedEvent : Event
{
  explicit ExitedEvent(const UPID& _pid)
    : pid(_pid) {}

  virtual void visit(EventVisitor* visitor) const
  {
    visitor->visit(*this);
  }

  const UPID pid;

private:
  // Keep ExitedEvent not assignable even though we made it copyable.
  // Note that we are violating the "rule of three" here but it helps
  // keep the fields const.
  ExitedEvent& operator=(const ExitedEvent&);
};


struct TerminateEvent : Event
{
  explicit TerminateEvent(const UPID& _from)
    : from(_from) {}

  virtual void visit(EventVisitor* visitor) const
  {
    visitor->visit(*this);
  }

  const UPID from;

private:
  // Not copyable, not assignable.
  TerminateEvent(const TerminateEvent&);
  TerminateEvent& operator=(const TerminateEvent&);
};

} // namespace process {

#endif // __PROCESS_EVENT_HPP__
