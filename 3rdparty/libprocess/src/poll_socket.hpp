#include <memory>

#include <process/socket.hpp>

#include <stout/try.hpp>

namespace process {
namespace network {

class PollSocketImpl : public Socket::Impl
{
public:
  static Try<std::shared_ptr<Socket::Impl>> create(int s);

  PollSocketImpl(int s) : Socket::Impl(s) {}

  virtual ~PollSocketImpl() {}

  // Implementation of the Socket::Impl interface.
  virtual Try<Nothing> listen(int backlog);
  virtual Future<Socket> accept();
  virtual Future<Nothing> connect(const Address& address);
  virtual Future<size_t> recv(char* data, size_t size);
  virtual Future<size_t> send(const char* data, size_t size);
  virtual Future<size_t> sendfile(int fd, off_t offset, size_t size);

  virtual Socket::Kind kind() const { return Socket::POLL; }
};

} // namespace network {
} // namespace process {
