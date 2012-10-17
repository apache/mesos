#ifndef __ENCODER_HPP__
#define __ENCODER_HPP__

#include <ev.h>

#include <sstream>

#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/numify.hpp>


namespace process {

typedef void (*Sender)(struct ev_loop*, ev_io*, int);

extern void send_data(struct ev_loop*, ev_io*, int);
extern void send_file(struct ev_loop*, ev_io*, int);


class Encoder
{
public:
  virtual ~Encoder() {}
  virtual Sender sender() = 0;
};


class DataEncoder : public Encoder
{
public:
  DataEncoder(const std::string& _data)
    : data(_data), index(0) {}

  virtual ~DataEncoder() {}

  virtual Sender sender()
  {
    return send_data;
  }

  virtual const char* next(size_t* length)
  {
    size_t temp = index;
    index = data.size();
    *length = data.size() - temp;
    return data.data() + temp;
  }

  virtual void backup(size_t length)
  {
    if (index >= length) {
      index -= length;
    }
  }

  virtual size_t remaining() const
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
  HttpResponseEncoder(const http::Response& response)
    : DataEncoder(encode(response)) {}

  static std::string encode(const http::Response& response)
  {
    std::ostringstream out;

    // TODO(benh): Check version?

    out << "HTTP/1.1 " << response.status << "\r\n";

    hashmap<std::string, std::string> headers = response.headers;

    // HTTP 1.1 requires the "Date" header. In the future once we
    // start checking the version (above) then we can conditionally
    // add this header, but for now, we always do.
    time_t rawtime;
    time(&rawtime);

    char date[256];

    // TODO(benh): Check return code of strftime!
    strftime(date, 256, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&rawtime));

    headers["Date"] = date;

    foreachpair (const std::string& key, const std::string& value, headers) {
      out << key << ": " << value << "\r\n";
    }

    // Add a Content-Length header if the response is of type "none"
    // or "body" and no Content-Length header has been supplied.
    if (response.type == http::Response::NONE &&
        !headers.contains("Content-Length")) {
      out << "Content-Length: 0\r\n";
    } else if (response.type == http::Response::BODY &&
               !headers.contains("Content-Length")) {
      out << "Content-Length: " << response.body.size() << "\r\n";
    }

    // Use a CRLF to mark end of headers.
    out << "\r\n";

    // Add the body if necessary.
    if (response.type == http::Response::BODY) {
      // If the Content-Length header was supplied, only write as much data
      // as the length specifies.
      Result<uint32_t> length = numify<uint32_t>(headers.get("Content-Length"));
      if (length.isSome() && length.get() <= response.body.length()) {
        out.write(response.body.data(), length.get());
      } else {
        out.write(response.body.data(), response.body.size());
      }
    }

    return out.str();
  }
};


class FileEncoder : public Encoder
{
public:
  FileEncoder(int _fd, size_t _size)
    : fd(_fd), size(_size), index(0) {}

  virtual ~FileEncoder()
  {
    close(fd);
  }

  virtual Sender sender()
  {
    return send_file;
  }

  virtual int next(off_t* offset, size_t* length)
  {
    off_t temp = index;
    index = size;
    *offset = temp;
    *length = size - temp;
    return fd;
  }

  virtual void backup(size_t length)
  {
    if (index >= length) {
      index -= length;
    }
  }

  virtual size_t remaining() const
  {
    return size - index;
  }

private:
  int fd;
  size_t size;
  off_t index;
};

}  // namespace process {

#endif // __ENCODER_HPP__
