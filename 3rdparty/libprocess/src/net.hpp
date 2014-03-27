/* TODO(benh): Write a form of 'Client' process. */

#ifndef NET_HPP
#define NET_HPP

#include <assert.h>
#include <errno.h>
#include <fcntl.h>

#include <process.hpp>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>

#include <stdexcept>
#include <iostream>

#include <stout/os.hpp>

typedef enum Protocol { TCP = SOCK_STREAM, UDP = SOCK_DGRAM } Protocol;

using std::runtime_error;
using std::string;


template <Protocol protocol>
class SocketProcess : public Process
{
protected:
  int s;

  void setsockopt(int level, int optname, const void *optval, socklen_t optlen)
  {
    if (::setsockopt(s, level, optname, optval, optlen) < 0)
      throw std::runtime_error(string("setsockopt: ") += strerror(errno));
  }

  virtual void socket()
  {
    if ((s = ::socket(AF_INET, protocol, IPPROTO_IP)) < 0)
      throw runtime_error(string("socket: ") += strerror(errno));

    socket(s);
  }

  virtual void socket(int sd)
  {
    s = sd;

    int flags = 1;
    if (ioctl(s, FIONBIO, &flags) &&
        ((flags = fcntl(s, F_GETFL, 0)) < 0 ||
          fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0))
      throw runtime_error(string("ioctl/fcntl: ") += strerror(errno));

    if (fcntl(s, F_SETFD, FD_CLOEXEC) < 0) {
      throw runtime_error(string("fcntl: ") += strerror(errno));
    }
  }

  virtual void bind(in_addr_t ip, in_port_t port)
  {
    struct sockaddr_in addr;
    addr.sin_family = PF_INET;
    addr.sin_addr.s_addr = ip;
    addr.sin_port = htons(port);

    if (::bind(s, (struct sockaddr *) &addr, sizeof(addr)) < 0)
      throw runtime_error(string("bind: ") += strerror(errno));
  }

  virtual ssize_t recv(void *buf, size_t bytes)
  {
    ssize_t len = 0;
    do {
      len = ::recv(s, buf, bytes, 0);

      if (len > 0)
        return len;
      else if (len < 0 && errno == EWOULDBLOCK)
        while (!await(s, RDONLY));
      else if (len == 0)
        throw runtime_error(string("recv: connection terminated"));
      else
        throw runtime_error(string("recv: ") += strerror(errno));
    } while (!(len > 0));

    return len;
  }

  virtual ssize_t recvall(void *buf, size_t bytes)
  {
    ssize_t len, offset = 0;
    do {
      len = ::recv(s, (char *) buf + offset, bytes - offset, 0);

      if (len > 0)
        offset += len;
      else if (len < 0 && errno == EWOULDBLOCK)
        while (!await(s, RDONLY));
      else if (len == 0)
        throw runtime_error(string("recvall: connection terminated"));
      else
        throw runtime_error(string("recvall: ") += strerror(errno));
    } while (offset != bytes);

    return offset;
  }

  virtual void send(const void *buf, size_t bytes)
  {
    size_t offset = 0;
    do {
      size_t len =
        ::send(s, (char *) buf + offset, bytes - offset, MSG_NOSIGNAL);

      if (len > 0)
        offset += len;
      else if (len < 0 && errno == EWOULDBLOCK)
        while (!await(s, WRONLY));
      else if (len == 0)
        throw runtime_error(string("send: connection terminated"));
      else
        throw runtime_error(string("send: ") += strerror(errno));
    } while (offset != bytes);
  }

  virtual void sendfile(int fd, size_t bytes)
  {
    off_t offset = 0;
    do {
      size_t len = ::sendfile(s, fd, 0, bytes - offset);

      if (len > 0)
        offset += len;
      else if (len < 0 && errno == EWOULDBLOCK)
        while (!await(s, WRONLY));
      else if (len == 0)
        throw runtime_error(string("sendfile: connection terminated"));
      else
        throw runtime_error(string("sendfile: ") += strerror(errno));
    } while (offset != bytes);
  }

public:
  SocketProcess() : s(-1) {}
  explicit SocketProcess(int _s) : s(_s)
  {
    int flags = 1;
    if (ioctl(s, FIONBIO, &flags) &&
        ((flags = fcntl(s, F_GETFL, 0)) < 0 ||
          fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0))
      throw runtime_error(string("ioctl/fcntl: ") += strerror(errno));
  }
  ~SocketProcess() { os::close(s); }
};


template <Protocol protocol>
class Acceptor : public SocketProcess<protocol>
{
protected:
  virtual int accept(struct sockaddr_in &addr)
  {
    int c;

    do {
      while (!await(SocketProcess<protocol>::s, Process::RDONLY));

      size_t size = sizeof(struct sockaddr_in);

      c = ::accept(
          SocketProcess<protocol>::s,
          (struct sockaddr *) &addr,
          (socklen_t *) &size);

      if (c == 0)
        throw runtime_error(string("accept: ") += strerror(errno));
      else if (c < 0 && (errno != EWOULDBLOCK))
        throw runtime_error(string("accept: ") += strerror(errno));
    } while (!(c > 0));

    return c;
  }

public:
  Acceptor() {}
  explicit Acceptor(int s) : SocketProcess<protocol>(s) {}
};


template <Protocol protocol>
class Server : public Acceptor<protocol>
{
protected:
  in_addr_t ip;
  in_port_t port;

  void init(in_addr_t _ip = INADDR_ANY, in_port_t _port = 0)
  {
    ip = _ip;
    port = _port;
    SocketProcess<protocol>::socket();
  }

  virtual void listen(int n)
  {
    int &s = SocketProcess<protocol>::s;
    if (::listen(s, n) < 0)
      throw runtime_error(string("listen: ") += strerror(errno));
  }

  virtual void bind()
  {
    SocketProcess<protocol>::bind(ip, port);
  }

public:
  Server(in_addr_t _ip = INADDR_ANY, in_port_t _port = 0)
    : ip(_ip), port(_port)
  {
    SocketProcess<protocol>::socket();
  }
};


#endif /* NET_HH */
