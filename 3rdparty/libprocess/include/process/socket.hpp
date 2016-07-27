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

#ifndef __PROCESS_SOCKET_HPP__
#define __PROCESS_SOCKET_HPP__

#ifndef __WINDOWS__
#include <sys/socket.h>
#include <sys/wait.h>
#endif // __WINDOWS__

#include <memory>

#include <process/address.hpp>
#include <process/future.hpp>

#include <stout/abort.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#ifdef __WINDOWS__
#include <stout/windows.hpp>
#endif // __WINDOWS__

namespace process {
namespace network {

/**
 * An abstraction around a socket (file descriptor).
 *
 * Provides reference counting such that the socket is only closed
 * (and thus, has the possiblity of being reused) after there are no
 * more references.
 */
class Socket
{
public:
  /**
   * Available kinds of implementations.
   *
   * @see process::network::PollSocketImpl
   * @see process::network::LibeventSSLSocketImpl
   */
  enum Kind
  {
    POLL,
#ifdef USE_SSL_SOCKET
    SSL
#endif
  };

  /**
   * Returns an instance of a `Socket` using the specified kind of
   * implementation. All implementations will set the NONBLOCK and
   * CLOEXEC options on the returned socket.
   *
   * @param kind Optional. The desired `Socket` implementation.
   * @param s Optional.  The file descriptor to wrap with the `Socket`.
   *
   * @return An instance of a `Socket`.
   *
   * TODO(josephw): MESOS-5729: Consider making the CLOEXEC option
   * configurable by the caller of the interface.
   */
  static Try<Socket> create(Kind kind = DEFAULT_KIND(), Option<int> s = None());

  /**
   * Returns the default `Kind` of implementation of `Socket`.
   *
   * @see process::network::Socket::Kind
   */
  static const Kind& DEFAULT_KIND();

  /**
   * Returns the kind representing the underlying implementation
   * of the `Socket` instance.
   *
   * @see process::network::Socket::Kind
   */
  Kind kind() const { return impl->kind(); }

  /**
   * Interface for a `Socket`.
   *
   * Each socket is:
   *   - reference counted,
   *   - shared by default,
   *   - and a concurrent object.
   *
   * Multiple implementations are supported via the Pimpl pattern,
   * rather than forcing each Socket implementation to do this themselves.
   *
   * @see process::network::Socket
   * @see [Pimpl pattern](https://en.wikipedia.org/wiki/Opaque_pointer)
   */
  class Impl : public std::enable_shared_from_this<Impl>
  {
  public:
    virtual ~Impl()
    {
      // Don't close if the socket was released.
      if (s >= 0) {
        CHECK_SOME(os::close(s)) << "Failed to close socket";
      }
    }

    /**
     * Returns the file descriptor wrapped by the `Socket`.
     */
    int get() const
    {
      return s;
    }

    /**
     * @copydoc network::address
     */
    Try<Address> address() const;

    /**
     * @copydoc network::peer
     */
    Try<Address> peer() const;

    /**
     * Assigns the specified address to the `Socket`.
     *
     * @return The assigned `Address` or an error if the bind system
     *     call fails.
     */
    Try<Address> bind(const Address& address);

    virtual Try<Nothing> listen(int backlog) = 0;

    /**
     * Returns a socket corresponding to the next pending connection
     * for the listening socket. All implementations will set the
     * NONBLOCK and CLOEXEC options on the returned socket.
     *
     * TODO(josephw): MESOS-5729: Consider making the CLOEXEC option
     * configurable by the caller of the interface.
     */
    virtual Future<Socket> accept() = 0;

    virtual Future<Nothing> connect(const Address& address) = 0;
    virtual Future<size_t> recv(char* data, size_t size) = 0;
    virtual Future<size_t> send(const char* data, size_t size) = 0;
    virtual Future<size_t> sendfile(int fd, off_t offset, size_t size) = 0;

    /**
     * An overload of `recv`, which receives data based on the specified
     * 'size' parameter.
     *
     * @param size
     *       Value  | Semantics
     *     :-------:|-----------
     *        0     | Returns an empty string.
     *       -1     | Receives until EOF.
     *        N     | Returns a string of size N.
     *       'None' | Returns a string of the available data.
     *     If 'None' is specified, whenever data becomes available on the
     *     socket, that much data will be returned.
     */
    // TODO(benh): Consider returning Owned<std::string> or
    // Shared<std::string>, the latter enabling reuse of a pool of
    // preallocated strings/buffers.
    virtual Future<std::string> recv(const Option<ssize_t>& size = None());

    /**
     * An overload of `send`, which sends all of the specified data.
     *
     * @param data The specified data to send.
     *
     * @return Nothing or an error in case the sending fails.
     */
    // TODO(benh): Consider taking Shared<std::string>, the latter
    // enabling reuse of a pool of preallocated strings/buffers.
    virtual Future<Nothing> send(const std::string& data);

    /**
     * Shutdown the receive-side of the socket. No further data can be
     * received from the socket.
     */
    // TODO(neilc): Change this to allow the caller to specify `how`.
    // See MESOS-5658.
    virtual Try<Nothing> shutdown()
    {
      if (::shutdown(s, SHUT_RD) < 0) {
        return ErrnoError();
      }

      return Nothing();
    }

    virtual Socket::Kind kind() const = 0;

    /**
     * Construct a new `Socket` from the given impl.
     *
     * This is a proxy function, as `Impl`s derived from this won't have
     * access to the Socket::Socket(...) constructors.
     */
    // TODO(jmlvanre): These should be protected; however, gcc complains
    // when using them from within a lambda of a derived class.
    static Socket socket(std::shared_ptr<Impl>&& that)
    {
      return Socket(std::move(that));
    }

    /**
     * @copydoc process::network::Socket::Impl::socket
     */
    static Socket socket(const std::shared_ptr<Impl>& that)
    {
      return Socket(that);
    }

  protected:
    explicit Impl(int _s) : s(_s) { CHECK(s >= 0); }

    /**
     * Releases ownership of the file descriptor. Not exposed
     * via the `Socket` interface as this is only intended to
     * support `Socket::Impl` implementations that need to
     * override the file descriptor ownership.
     */
    int release()
    {
      int released = s;
      s = -1;
      return released;
    }

    /**
     * Construct a `Socket` wrapper from this implementation.
     */
    Socket socket() { return Socket(shared_from_this()); }

    /**
     * Returns a `std::shared_ptr<T>` from this implementation.
     */
    template <typename T>
    static std::shared_ptr<T> shared(T* t)
    {
      std::shared_ptr<T> pointer =
        std::dynamic_pointer_cast<T>(CHECK_NOTNULL(t)->shared_from_this());
      CHECK(pointer);
      return pointer;
    }

    int s;
  };

  bool operator==(const Socket& that) const
  {
    return impl == that.impl;
  }

  operator int() const
  {
    return impl->get();
  }

  Try<Address> address() const
  {
    return impl->address();
  }

  Try<Address> peer() const
  {
    return impl->peer();
  }

  int get() const
  {
    return impl->get();
  }

  Try<Address> bind(const Address& address = Address::LOCALHOST_ANY())
  {
    return impl->bind(address);
  }

  Try<Nothing> listen(int backlog)
  {
    return impl->listen(backlog);
  }

  Future<Socket> accept()
  {
    return impl->accept();
  }

  Future<Nothing> connect(const Address& address)
  {
    return impl->connect(address);
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

  Future<std::string> recv(const Option<ssize_t>& size = None())
  {
    return impl->recv(size);
  }

  Future<Nothing> send(const std::string& data)
  {
    return impl->send(data);
  }

  Try<Nothing> shutdown()
  {
    return impl->shutdown();
  }

private:
  explicit Socket(std::shared_ptr<Impl>&& that) : impl(std::move(that)) {}

  explicit Socket(const std::shared_ptr<Impl>& that) : impl(that) {}

  std::shared_ptr<Impl> impl;
};

} // namespace network {
} // namespace process {

#endif // __PROCESS_SOCKET_HPP__
