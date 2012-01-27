#ifndef __PROCESS_EVENT_HPP__
#define __PROCESS_EVENT_HPP__

#include <tr1/functional>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/message.hpp>

namespace process {

// Forward declarations.
struct ProcessBase;
struct MessageEvent;
struct DispatchEvent;
struct HttpEvent;
struct ExitedEvent;
struct TerminateEvent;


struct EventVisitor
{
  virtual void visit(const MessageEvent& event) {}
  virtual void visit(const DispatchEvent& event) {}
  virtual void visit(const HttpEvent& event) {}
  virtual void visit(const ExitedEvent& event) {}
  virtual void visit(const TerminateEvent& event) {}
};


struct Event
{
  virtual void visit(EventVisitor* visitor) const = 0;

  template <typename T>
  bool is() const
  {
    bool result = false;
    struct IsVisitor : EventVisitor
    {
      IsVisitor(bool* _result) : result(_result) {}
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
      AsVisitor(const T** _result) : result(_result) {}
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
  MessageEvent(Message* _message)
    : message(_message) {}

  ~MessageEvent()
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
  HttpEvent(int _c, HttpRequest* _request)
    : c(_c), request(_request) {}

  ~HttpEvent()
  {
    delete request;
  }

  virtual void visit(EventVisitor* visitor) const
  {
    visitor->visit(*this);
  }

  const int c;
  HttpRequest* const request;

private:
  // Not copyable, not assignable.
  HttpEvent(const HttpEvent&);
  HttpEvent& operator = (const HttpEvent&);
};


struct DispatchEvent : Event
{
  DispatchEvent(std::tr1::function<void(ProcessBase*)>* _function)
    : function(_function) {}

  ~DispatchEvent()
  {
    delete function;
  }

  virtual void visit(EventVisitor* visitor) const
  {
    visitor->visit(*this);
  }

  std::tr1::function<void(ProcessBase*)>* const function;

private:
  // Not copyable, not assignable.
  DispatchEvent(const DispatchEvent&);
  DispatchEvent& operator = (const DispatchEvent&);
};


struct ExitedEvent : Event
{
  ExitedEvent(const UPID& _pid)
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
  TerminateEvent(const UPID& _from)
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
