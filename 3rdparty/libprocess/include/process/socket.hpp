#ifndef __PROCESS_SOCKET_HPP__
#define __PROCESS_SOCKET_HPP__

#include <memory>

#include <process/future.hpp>
#include <process/node.hpp>

#include <stout/abort.hpp>
#include <stout/nothing.hpp>
#include <stout/net.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

namespace process {
namespace network {

// An abstraction around a socket (file descriptor) that provides
// reference counting such that the socket is only closed (and thus,
// has the possiblity of being reused) after there are no more
// references.
class Socket
{
public:
  // Available kinds of implementations.
  enum Kind {
    POLL,
    // TODO(jmlvanre): Add libevent SSL socket.
  };

  // Returns an instance of a Socket using the specified kind of
  // implementation and potentially wrapping the specified file
  // descriptor.
  static Try<Socket> create(Kind kind = DEFAULT_KIND(), int s = -1);

  // Returns the default kind of implementation of Socket.
  static const Kind& DEFAULT_KIND();

  // Each socket is a reference counted, shared by default, concurrent
  // object. However, since we want to support multiple
  // implementations we use the Pimpl pattern (often called the
  // compilation firewall) rather than forcing each Socket
  // implementation to do this themselves.
  class Impl : public std::enable_shared_from_this<Impl>
  {
  public:
    virtual ~Impl()
    {
      CHECK(s >= 0);
      Try<Nothing> close = os::close(s);
      if (close.isError()) {
        ABORT("Failed to close socket " +
              stringify(s) + ": " + close.error());
      }
    }

    int get() const
    {
      return s;
    }

    // Socket::Impl interface.
    virtual Try<Node> bind(const Node& node);
    virtual Try<Nothing> listen(int backlog) = 0;
    virtual Future<Socket> accept() = 0;
    virtual Future<Nothing> connect(const Node& node) = 0;
    virtual Future<size_t> recv(char* data, size_t size) = 0;
    virtual Future<size_t> send(const char* data, size_t size) = 0;
    virtual Future<size_t> sendfile(int fd, off_t offset, size_t size) = 0;

  protected:
    explicit Impl(int _s) : s(_s) { CHECK(s >= 0); }

    // Construct a Socket wrapper from this implementation.
    Socket socket() { return Socket(shared_from_this()); }

    int s;
  };

  bool operator == (const Socket& that) const
  {
    return impl == that.impl;
  }

  operator int () const
  {
    return impl->get();
  }

  int get() const
  {
    return impl->get();
  }

  Try<Node> bind(const Node& node)
  {
    return impl->bind(node);
  }

  Try<Nothing> listen(int backlog)
  {
    return impl->listen(backlog);
  }

  Future<Socket> accept()
  {
    return impl->accept();
  }

  Future<Nothing> connect(const Node& node)
  {
    return impl->connect(node);
  }

  Future<size_t> recv(char* data, size_t size) const
  {
    return impl->recv(data, size);
  }

  Future<size_t> send(const char* data, size_t size) const
  {
    return impl->send(data, size);
  }

  Future<size_t> sendfile(int fd, off_t offset, size_t size) const
  {
    return impl->sendfile(fd, offset, size);
  }

private:
  explicit Socket(std::shared_ptr<Impl>&& that) : impl(std::move(that)) {}

  explicit Socket(const std::shared_ptr<Impl>& that) : impl(that) {}

  std::shared_ptr<Impl> impl;
};

} // namespace network {
} // namespace process {

#endif // __PROCESS_SOCKET_HPP__
