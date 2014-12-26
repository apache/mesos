#include <arpa/inet.h>

#include <stdint.h>

#include <cstring>
#include <deque>
#include <iostream>
#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "decoder.hpp"

using std::deque;
using std::string;

using process::http::Request;
using process::http::Response;

using process::network::Socket;

namespace process {

namespace http {

hashmap<uint16_t, string> statuses;

namespace query {

Try<hashmap<std::string, std::string>> decode(const std::string& query)
{
  hashmap<std::string, std::string> result;

  const std::vector<std::string>& tokens = strings::tokenize(query, ";&");
  foreach (const std::string& token, tokens) {
    const std::vector<std::string>& pairs = strings::split(token, "=", 2);
    if (pairs.size() == 0) {
      continue;
    }

    Try<std::string> key = http::decode(pairs[0]);
    if (key.isError()) {
      return Error(key.error());
    }

    if (pairs.size() == 2) {
      Try<std::string> value = http::decode(pairs[1]);
      if (value.isError()) {
        return Error(value.error());
      }
      result[key.get()] = value.get();

    } else if (pairs.size() == 1) {
      result[key.get()] = "";
    }
  }

  return result;
}


std::string encode(const hashmap<std::string, std::string>& query)
{
  std::string output;

  foreachpair (const std::string& key, const std::string& value, query) {
    output += http::encode(key);
    if (!value.empty()) {
      output += "=" + http::encode(value);
    }
    output += '&';
  }
  return strings::remove(output, "&", strings::SUFFIX);
}

} // namespace query {

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


// Forward declaration.
Future<Response> _request(
    Socket socket,
    const UPID& upid,
    const string& method,
    const Option<string>& path,
    const Option<string>& query,
    const Option<hashmap<string, string> >& _headers,
    const Option<string>& body,
    const Option<string>& contentType);


Future<Response> request(
    const UPID& upid,
    const string& method,
    const Option<string>& path,
    const Option<string>& query,
    const Option<hashmap<string, string> >& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  Try<Socket> create = Socket::create();

  if (create.isError()) {
    return Failure("Failed to create socket: " + create.error());
  }

  Socket socket = create.get();

  return socket.connect(upid.node)
    .then(lambda::bind(&_request,
                       socket,
                       upid,
                       method,
                       path,
                       query,
                       headers,
                       body,
                       contentType));
}


Future<Response> _request(
    Socket socket,
    const UPID& upid,
    const string& method,
    const Option<string>& path,
    const Option<string>& query,
    const Option<hashmap<string, string> >& _headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
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
  headers["Host"] = stringify(upid.node);

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

  // Need to disambiguate the Socket::recv for binding below.
  Future<string> (Socket::*recv)(const Option<ssize_t>&) = &Socket::recv;

  // TODO(bmahler): For efficiency, this should properly use the
  // ResponseDecoder when reading, rather than parsing the full string
  // response.
  return socket.send(out.str())
    .then(lambda::function<Future<string>(void)>(
              lambda::bind(recv, socket, -1)))
    .then(lambda::bind(&internal::decode, lambda::_1));
}


} // namespace internal {


Future<Response> get(
    const UPID& upid,
    const Option<string>& path,
    const Option<string>& query,
    const Option<hashmap<string, string> >& headers)
{
  URL url("http", net::IP(upid.address.ip), upid.address.port, upid.id);

  if (path.isSome()) {
    // TODO(benh): Get 'query' and/or 'fragment' out of 'path'.
    url.path = strings::join("/", url.path, path.get());
  }

  if (query.isSome()) {
    Try<hashmap<string, string>> decode = http::query::decode(
        strings::remove(query.get(), "?", strings::PREFIX));

    if (decode.isError()) {
      return Failure("Failed to decode HTTP query string: " + decode.error());
    }

    url.query = decode.get();
  }

  return get(url, headers);
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
