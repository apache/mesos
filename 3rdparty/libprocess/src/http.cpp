#include <arpa/inet.h>

#include <stdint.h>

#include <cstring>
#include <deque>
#include <iostream>
#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/socket.hpp>

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include "decoder.hpp"

using std::deque;
using std::string;

using process::http::Request;
using process::http::Response;

namespace process {

namespace http {

hashmap<uint16_t, string> statuses;

namespace internal {

Future<Response> decode(const string& buffer)
{
  ResponseDecoder decoder;
  deque<Response*> responses = decoder.decode(buffer.c_str(), buffer.length());

  if (decoder.failed() || responses.empty()) {
    for (size_t i = 0; i < responses.size(); ++i) {
      delete responses[i];
    }
    return Failure("Failed to decode HTTP response:\n" + buffer + "\n");
  } else if (responses.size() > 1) {
    PLOG(ERROR) << "Received more than 1 HTTP Response";
  }

  Response response = *responses[0];
  for (size_t i = 0; i < responses.size(); ++i) {
    delete responses[i];
  }

  return response;
}


Future<Response> request(
    const UPID& upid,
    const string& method,
    const Option<string>& path,
    const Option<string>& query,
    const Option<hashmap<string, string> >& _headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  Try<int> socket = network::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

  if (socket.isError()) {
    return Failure("Failed to create socket: " + socket.error());
  }

  int s = socket.get();

  Try<Nothing> cloexec = os::cloexec(s);
  if (!cloexec.isSome()) {
    os::close(s);
    return Failure("Failed to cloexec: " + cloexec.error());
  }

  const string host = stringify(upid.node);

  Try<int> connect = network::connect(s, upid.node);
  if (connect.isError()) {
    os::close(s);
    return Failure(connect.error());
  }

  std::ostringstream out;

  out << method << " /" << upid.id;

  if (path.isSome()) {
    out << "/" << path.get();
  }

  if (query.isSome()) {
    out << "?" << query.get();
  }

  out << " HTTP/1.1\r\n";

  // Set up the headers as necessary.
  hashmap<string, string> headers;

  if (_headers.isSome()) {
    headers = _headers.get();
  }

  // Need to specify the 'Host' header.
  headers["Host"] = host;

  // Tell the server to close the connection when it's done.
  headers["Connection"] = "close";

  // Overwrite Content-Type if necessary.
  if (contentType.isSome()) {
    headers["Content-Type"] = contentType.get();
  }

  // Make sure the Content-Length is set correctly if necessary.
  if (body.isSome()) {
    headers["Content-Length"] = stringify(body.get().length());
  }

  // Emit the headers.
  foreachpair (const string& key, const string& value, headers) {
    out << key << ": " << value << "\r\n";
  }

  out << "\r\n";

  if (body.isSome()) {
    out << body.get();
  }

  Try<Nothing> nonblock = os::nonblock(s);
  if (!nonblock.isSome()) {
    os::close(s);
    return Failure("Failed to set nonblock: " + nonblock.error());
  }

  // Need to disambiguate the io::read we want when binding below.
  Future<string> (*read)(int) = io::read;

  return io::write(s, out.str())
    .then(lambda::bind(read, s))
    .then(lambda::bind(&internal::decode, lambda::_1))
    .onAny(lambda::bind(&os::close, s));
}


} // namespace internal {


Future<Response> get(
    const UPID& upid,
    const Option<string>& path,
    const Option<string>& query,
    const Option<hashmap<string, string> >& headers)
{
  return internal::request(
      upid, "GET", path, query, headers, None(), None());
}


Future<Response> post(
    const UPID& upid,
    const Option<string>& path,
    const Option<hashmap<string, string> >& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  if (body.isNone() && contentType.isSome()) {
    return Failure("Attempted to do a POST with a Content-Type but no body");
  }

  return internal::request(
      upid, "POST", path, None(), headers, body, contentType);
}


} // namespace http {
} // namespace process {
