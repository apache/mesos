#ifndef __ENCODER_HPP__
#define __ENCODER_HPP__

#include <sstream>

#include "process.hpp"


class DataEncoder
{
public:
  DataEncoder(const std::string& _data)
    : data(_data), index(0) {}

  virtual ~DataEncoder() {}

  const char* next(size_t* length)
  {
    size_t temp = index;
    index = data.size();
    *length = data.size() - temp;
    return data.data() + temp;
  }

  void backup(size_t length)
  {
    if (index >= length) {
      index -= length;
    }
  }

  size_t remaining() const
  {
    return data.size() - index;
  }

private:
  const std::string data;
  size_t index;
};


class MessageEncoder : public DataEncoder
{
public:
  MessageEncoder(Message* _message)
    : DataEncoder(encode(_message)), message(_message) {}

  virtual ~MessageEncoder()
  {
    if (message != NULL) {
      delete message;
    }
  }

  static std::string encode(Message* message)
  {
    if (message != NULL) {
      std::ostringstream out;

      out << "POST /" << message->to.id << "/" << message->name
          << " HTTP/1.0\r\n"
          << "User-Agent: libprocess/" << message->from << "\r\n"
          << "Connection: Keep-Alive\r\n";

      if (message->body.size() > 0) {
        out << "Transfer-Encoding: chunked\r\n\r\n"
            << std::hex << message->body.size() << "\r\n";
        out.write(message->body.data(), message->body.size());
        out << "\r\n"
            << "0\r\n"
            << "\r\n";
      } else {
        out << "\r\n";
      }

      return out.str();
    }
  }

private:
  Message* message;
};


class HttpResponseEncoder : public DataEncoder
{
public:
  HttpResponseEncoder(const std::string& response)
    : DataEncoder(encode(response)) {}

  static std::string encode(const std::string& response)
  {
    std::ostringstream out;

    out << "HTTP/1.1 200 OK\r\n"
//         << "Content-Type: text/html\r\n"
        << "Content-Length: " << response.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    out.write(response.data(), response.size());

    return out.str();
  }
};


class HttpGatewayTimeoutEncoder : public DataEncoder
{
public:
  HttpGatewayTimeoutEncoder()
    : DataEncoder(encode()) {}

  static std::string encode()
  {
    std::ostringstream out;

    // TODO(benh): We send a content length of 0 so that a browser
    // won't just sit and wait for the socket to get closed (because
    // we can't close it our self right now).
    out << "HTTP/1.1 504 Gateway Timeout\r\n"
        << "Content-Length: 0\r\n"
        << "Connection: close\r\n"
        << "\r\n";

    return out.str();
  }
};


#endif // __ENCODER_HPP__
