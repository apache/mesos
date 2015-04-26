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
  virtual void visit(const MessageEvent& event) {}
  virtual void visit(const DispatchEvent& event) {}
  virtual void visit(const HttpEvent& event) {}
  virtual void visit(const ExitedEvent& event) {}
  virtual void visit(const TerminateEvent& event) {}
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
      virtual void visit(const T& t) { *result = true; }
      bool* result;
    } visitor(&result);
    visit(&visitor);
    return result;
  }

  template <typename T>
  const T& as() const
  {
    const T* result = NULL;
    struct AsVisitor : EventVisitor
    {
      explicit AsVisitor(const T** _result) : result(_result) {}
      virtual void visit(const T& t) { *result = &t; }
      const T** result;
    } visitor(&result);
    visit(&visitor);
    if (result == NULL) {
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
    : message(that.message == NULL ? NULL : new Message(*that.message)) {}

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
  MessageEvent& operator = (const MessageEvent&);
};


struct HttpEvent : Event
{
  HttpEvent(const network::Socket& _socket, http::Request* _request)
    : socket(_socket), request(_request) {}

  virtual ~HttpEvent()
  {
    delete request;
  }

  virtual void visit(EventVisitor* visitor) const
  {
    visitor->visit(*this);
  }

  const network::Socket socket;
  http::Request* const request;

private:
  // Not copyable, not assignable.
  HttpEvent(const HttpEvent&);
  HttpEvent& operator = (const HttpEvent&);
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
  DispatchEvent& operator = (const DispatchEvent&);
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
  ExitedEvent& operator = (const ExitedEvent&);
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
  TerminateEvent& operator = (const TerminateEvent&);
};

} // namespace process {

#endif // __PROCESS_EVENT_HPP__
