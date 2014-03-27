#ifndef __PROCESS_EVENT_HPP__
#define __PROCESS_EVENT_HPP__

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/message.hpp>
#include <process/socket.hpp>

#include <stout/lambda.hpp>
#include <stout/memory.hpp> // TODO(benh): Replace shared_ptr with unique_ptr.

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
      std::cerr << "Attempting to \"cast\" event incorrectly!" << std::endl;
      abort();
    }
    return *result;
  }
};


struct MessageEvent : Event
{
  explicit MessageEvent(Message* _message)
    : message(_message) {}

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
  // Not copyable, not assignable.
  MessageEvent(const MessageEvent&);
  MessageEvent& operator = (const MessageEvent&);
};


struct HttpEvent : Event
{
  HttpEvent(const Socket& _socket, http::Request* _request)
    : socket(_socket), request(_request) {}

  virtual ~HttpEvent()
  {
    delete request;
  }

  virtual void visit(EventVisitor* visitor) const
  {
    visitor->visit(*this);
  }

  const Socket socket;
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
      const memory::shared_ptr<lambda::function<void(ProcessBase*)> >& _f,
      const std::string& _method)
    : pid(_pid),
      f(_f),
      method(_method)
  {}

  virtual void visit(EventVisitor* visitor) const
  {
    visitor->visit(*this);
  }

  // PID receiving the dispatch.
  const UPID pid;

  // Function to get invoked as a result of this dispatch event.
  const memory::shared_ptr<lambda::function<void(ProcessBase*)> > f;

  // Canonical "byte" representation of a pointer to a member function
  // (i.e., method) encapsulated in the above function (or empty if
  // not applicable). Note that we use a byte representation because a
  // pointer to a member function is not actually a pointer, but
  // instead a POD.
  // TODO(benh): Perform canonicalization lazily.
  const std::string method;

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
  // Not copyable, not assignable.
  ExitedEvent(const ExitedEvent&);
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

} // namespace event {

#endif // __PROCESS_EVENT_HPP__
