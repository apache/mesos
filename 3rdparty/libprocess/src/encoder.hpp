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

#ifndef __ENCODER_HPP__
#define __ENCODER_HPP__

#include <stdint.h>
#include <time.h>

#include <limits>
#include <map>
#include <sstream>

#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/gzip.hpp>
#include <stout/hashmap.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>


namespace process {

const uint32_t GZIP_MINIMUM_BODY_LENGTH = 1024;


class Encoder
{
public:
  enum Kind
  {
    DATA,
    FILE
  };

  Encoder() = default;

  virtual ~Encoder() {}

  virtual Kind kind() const = 0;

  virtual void backup(size_t length) = 0;

  virtual size_t remaining() const = 0;
};


class DataEncoder : public Encoder
{
public:
  DataEncoder(const std::string& _data)
    : data(_data), index(0) {}

  DataEncoder(std::string&& _data)
    : data(std::move(_data)), index(0) {}

  ~DataEncoder() override {}

  Kind kind() const override
  {
    return Encoder::DATA;
  }

  virtual const char* next(size_t* length)
  {
    size_t temp = index;
    index = data.size();
    *length = data.size() - temp;
    return data.data() + temp;
  }

  void backup(size_t length) override
  {
    if (index >= length) {
      index -= length;
    }
  }

  size_t remaining() const override
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
  MessageEncoder(const Message& message)
    : DataEncoder(encode(message)) {}

  static std::string encode(const Message& message)
  {
    std::ostringstream out;

    out << "POST ";
    // Nothing keeps the 'id' component of a PID from being an empty
    // string which would create a malformed path that has two
    // '//' unless we check for it explicitly.
    // TODO(benh): Make the 'id' part of a PID optional so when it's
    // missing it's clear that we're simply addressing an ip:port.
    if (message.to.id != "") {
      out << "/" << message.to.id;
    }

    out << "/" << message.name << " HTTP/1.1\r\n"
        << "User-Agent: libprocess/" << message.from << "\r\n"
        << "Libprocess-From: " << message.from << "\r\n"
        << "Connection: Keep-Alive\r\n"
        << "Host: " << message.to.address << "\r\n";

    if (message.body.size() > 0) {
      out << "Transfer-Encoding: chunked\r\n\r\n"
          << std::hex << message.body.size() << "\r\n";
      out.write(message.body.data(), message.body.size());
      out << "\r\n"
          << "0\r\n"
          << "\r\n";
    } else {
      out << "\r\n";
    }

    return out.str();
  }
};


class HttpResponseEncoder : public DataEncoder
{
public:
  HttpResponseEncoder(
      const http::Response& response,
      const http::Request& request)
    : DataEncoder(encode(response, request)) {}

  static std::string encode(
      const http::Response& response,
      const http::Request& request)
  {
    std::ostringstream out;

    // TODO(benh): Check version?

    out << "HTTP/1.1 " << response.status << "\r\n";

    auto headers = response.headers;

    // HTTP 1.1 requires the "Date" header. In the future once we
    // start checking the version (above) then we can conditionally
    // add this header, but for now, we always do.
    time_t rawtime;
    time(&rawtime);

    char date[256];

    tm tm_;
    PCHECK(os::gmtime_r(&rawtime, &tm_) != nullptr)
      << "Failed to convert the current time to a tm struct "
      << "using os::gmtime_r()";

    // TODO(benh): Check return code of strftime!
    strftime(date, 256, "%a, %d %b %Y %H:%M:%S GMT", &tm_);

    headers["Date"] = date;

    // Should we compress this response?
    std::string body = response.body;

    if (response.type == http::Response::BODY &&
        response.body.length() >= GZIP_MINIMUM_BODY_LENGTH &&
        !headers.contains("Content-Encoding") &&
        request.acceptsEncoding("gzip")) {
      Try<std::string> compressed = gzip::compress(body);
      if (compressed.isError()) {
        LOG(WARNING) << "Failed to gzip response body: " << compressed.error();
      } else {
        body = std::move(compressed.get());

        headers["Content-Length"] = stringify(body.length());
        headers["Content-Encoding"] = "gzip";
      }
    }

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
      out << "Content-Length: " << body.size() << "\r\n";
    }

    // Use a CRLF to mark end of headers.
    out << "\r\n";

    // Add the body if necessary.
    if (response.type == http::Response::BODY) {
      // If the Content-Length header was supplied, only write as much data
      // as the length specifies.
      Result<uint32_t> length = numify<uint32_t>(headers.get("Content-Length"));
      if (length.isSome() && length.get() <= body.length()) {
        out.write(body.data(), length.get());
      } else {
        out.write(body.data(), body.size());
      }
    }

    return out.str();
  }
};


class FileEncoder : public Encoder
{
public:
  FileEncoder(int_fd _fd, size_t _size)
    : fd(_fd), size(static_cast<off_t>(_size)), index(0)
  {
    // NOTE: For files, we expect the size to be derived from `stat`-ing
    // the file.  The `struct stat` returns the size in `off_t` form,
    // meaning that it is a programmer error to construct the `FileEncoder`
    // with a size greater the max value of `off_t`.
    CHECK_LE(_size, static_cast<size_t>(std::numeric_limits<off_t>::max()));
  }

  ~FileEncoder() override
  {
    CHECK_SOME(os::close(fd)) << "Failed to close file descriptor";
  }

  Kind kind() const override
  {
    return Encoder::FILE;
  }

  virtual int_fd next(off_t* offset, size_t* length)
  {
    off_t temp = index;
    index = size;
    *offset = temp;
    *length = size - temp;
    return fd;
  }

  void backup(size_t length) override
  {
    if (index >= static_cast<off_t>(length)) {
      index -= static_cast<off_t>(length);
    }
  }

  size_t remaining() const override
  {
    return static_cast<size_t>(size - index);
  }

private:
  int_fd fd;
  off_t size;
  off_t index;
};

}  // namespace process {

#endif // __ENCODER_HPP__
