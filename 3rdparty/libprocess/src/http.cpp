/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#include <arpa/inet.h>

#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <iomanip>
#include <ostream>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <sstream>
#include <vector>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>

#include <stout/foreach.hpp>
#include <stout/ip.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/strings.hpp>
#include <stout/synchronized.hpp>
#include <stout/try.hpp>

#include "decoder.hpp"

using std::deque;
using std::istringstream;
using std::map;
using std::ostream;
using std::ostringstream;
using std::queue;
using std::string;
using std::vector;

using process::http::Request;
using process::http::Response;

using process::network::Address;
using process::network::Socket;

namespace process {
namespace http {


hashmap<uint16_t, string> statuses;


void initialize()
{
  statuses[100] = "100 Continue";
  statuses[101] = "101 Switching Protocols";
  statuses[200] = "200 OK";
  statuses[201] = "201 Created";
  statuses[202] = "202 Accepted";
  statuses[203] = "203 Non-Authoritative Information";
  statuses[204] = "204 No Content";
  statuses[205] = "205 Reset Content";
  statuses[206] = "206 Partial Content";
  statuses[300] = "300 Multiple Choices";
  statuses[301] = "301 Moved Permanently";
  statuses[302] = "302 Found";
  statuses[303] = "303 See Other";
  statuses[304] = "304 Not Modified";
  statuses[305] = "305 Use Proxy";
  statuses[307] = "307 Temporary Redirect";
  statuses[400] = "400 Bad Request";
  statuses[401] = "401 Unauthorized";
  statuses[402] = "402 Payment Required";
  statuses[403] = "403 Forbidden";
  statuses[404] = "404 Not Found";
  statuses[405] = "405 Method Not Allowed";
  statuses[406] = "406 Not Acceptable";
  statuses[407] = "407 Proxy Authentication Required";
  statuses[408] = "408 Request Time-out";
  statuses[409] = "409 Conflict";
  statuses[410] = "410 Gone";
  statuses[411] = "411 Length Required";
  statuses[412] = "412 Precondition Failed";
  statuses[413] = "413 Request Entity Too Large";
  statuses[414] = "414 Request-URI Too Large";
  statuses[415] = "415 Unsupported Media Type";
  statuses[416] = "416 Requested range not satisfiable";
  statuses[417] = "417 Expectation Failed";
  statuses[500] = "500 Internal Server Error";
  statuses[501] = "501 Not Implemented";
  statuses[502] = "502 Bad Gateway";
  statuses[503] = "503 Service Unavailable";
  statuses[504] = "504 Gateway Time-out";
  statuses[505] = "505 HTTP Version not supported";
}


bool Request::acceptsEncoding(const string& encoding) const
{
  // From RFC 2616:
  //
  // 1. If the content-coding is one of the content-codings listed in
  //    the Accept-Encoding field, then it is acceptable, unless it is
  //    accompanied by a qvalue of 0. (As defined in section 3.9, a
  //    qvalue of 0 means "not acceptable.")
  //
  // 2. The special "*" symbol in an Accept-Encoding field matches any
  //    available content-coding not explicitly listed in the header
  //    field.
  //
  // 3. If multiple content-codings are acceptable, then the acceptable
  //    content-coding with the highest non-zero qvalue is preferred.
  //
  // 4. The "identity" content-coding is always acceptable, unless
  //    specifically refused because the Accept-Encoding field includes
  //    "identity;q=0", or because the field includes "*;q=0" and does
  //    not explicitly include the "identity" content-coding. If the
  //    Accept-Encoding field-value is empty, then only the "identity"
  //    encoding is acceptable.
  //
  // If no Accept-Encoding field is present in a request, the server
  // MAY assume that the client will accept any content coding. In
  // this case, if "identity" is one of the available content-codings,
  // then the server SHOULD use the "identity" content-coding...
  Option<string> accept = headers.get("Accept-Encoding");

  if (accept.isNone() || accept.get().empty()) {
    return false;
  }

  // Remove spaces and tabs for easier parsing.
  accept = strings::remove(accept.get(), " ");
  accept = strings::remove(accept.get(), "\t");
  accept = strings::remove(accept.get(), "\n");

  // First we'll look for the encoding specified explicitly, then '*'.
  vector<string> candidates;
  candidates.push_back(encoding);      // Rule 1.
  candidates.push_back("*");           // Rule 2.

  foreach (const string& candidate, candidates) {
    // Is the candidate one of the accepted encodings?
    foreach (const string& encoding_, strings::tokenize(accept.get(), ",")) {
      vector<string> tokens = strings::tokenize(encoding_, ";");

      if (tokens.empty()) {
        continue;
      }

      if (strings::lower(tokens[0]) == strings::lower(candidate)) {
        // Is there a 0 q value? Ex: 'gzip;q=0.0'.
        const map<string, vector<string>> values =
          strings::pairs(encoding_, ";", "=");

        // Look for { "q": ["0"] }.
        if (values.count("q") == 0 || values.find("q")->second.size() != 1) {
          // No q value, or malformed q value.
          return true;
        }

        // Is the q value > 0?
        Try<double> value = numify<double>(values.find("q")->second[0]);
        return value.isSome() && value.get() > 0;
      }
    }
  }

  return false;
}


bool Request::acceptsMediaType(const string& mediaType) const
{
  vector<string> mediaTypes = strings::tokenize(mediaType, "/");

  if (mediaTypes.size() != 2) {
    return false;
  }

  Option<string> accept = headers.get("Accept");

  // If no Accept header field is present, then it is assumed
  // that the client accepts all media types.
  if (accept.isNone()) {
    return true;
  }

  // Remove spaces and tabs for easier parsing.
  accept = strings::remove(accept.get(), " ");
  accept = strings::remove(accept.get(), "\t");
  accept = strings::remove(accept.get(), "\n");

  // First match 'type/subtype', then 'type/*', then '*/*'.
  vector<string> candidates;
  candidates.push_back(mediaType);
  candidates.push_back(mediaTypes[0] + "/*");
  candidates.push_back("*/*");

  foreach (const string& candidate, candidates) {
    foreach (const string& type, strings::tokenize(accept.get(), ",")) {
      vector<string> tokens = strings::tokenize(type, ";");

      if (tokens.empty()) {
        continue;
      }

      // Is the candidate one of the accepted type?
      if (strings::lower(tokens[0]) == strings::lower(candidate)) {
        // Is there a 0 q value? Ex: 'gzip;q=0.0'.
        const map<string, vector<string>> values =
          strings::pairs(type, ";", "=");

        // Look for { "q": ["0"] }.
        if (values.count("q") == 0 || values.find("q")->second.size() != 1) {
          // No q value, or malformed q value.
          return true;
        }

        // Is the q value > 0?
        Try<double> value = numify<double>(values.find("q")->second[0]);
        return value.isSome() && value.get() > 0;
      }
    }
  }

  return false;
}


Pipe::Reader Pipe::reader() const
{
  return Pipe::Reader(data);
}


Pipe::Writer Pipe::writer() const
{
  return Pipe::Writer(data);
}


Future<string> Pipe::Reader::read()
{
  Future<string> future;

  synchronized (data->lock) {
    if (data->readEnd == Reader::CLOSED) {
      future = Failure("closed");
    } else if (!data->writes.empty()) {
      future = data->writes.front();
      data->writes.pop();
    } else if (data->writeEnd == Writer::CLOSED) {
      future = ""; // End-of-file.
    } else if (data->writeEnd == Writer::FAILED) {
      CHECK_SOME(data->failure);
      future = data->failure.get();
    } else {
      data->reads.push(Owned<Promise<string>>(new Promise<string>()));
      future = data->reads.back()->future();
    }
  }

  return future;
}


bool Pipe::Reader::close()
{
  bool closed = false;
  bool notify = false;
  queue<Owned<Promise<string>>> reads;

  synchronized (data->lock) {
    if (data->readEnd == Reader::OPEN) {
      // Throw away outstanding data.
      while (!data->writes.empty()) {
        data->writes.pop();
      }

      // Extract the pending reads so we can fail them.
      std::swap(data->reads, reads);

      closed = true;
      data->readEnd = Reader::CLOSED;

      // Notify if write-end is still open!
      notify = data->writeEnd == Writer::OPEN;
    }
  }

  // NOTE: We transition the promises outside the critical section
  // to avoid triggering callbacks that try to reacquire the lock.
  if (closed) {
    while (!reads.empty()) {
      reads.front()->fail("closed");
      reads.pop();
    }

    if (notify) {
      data->readerClosure.set(Nothing());
    }
  }

  return closed;
}


bool Pipe::Writer::write(const string& s)
{
  bool written = false;
  Owned<Promise<string>> read;

  synchronized (data->lock) {
    // Ignore writes if either end of the pipe is closed or failed!
    if (data->writeEnd == Writer::OPEN && data->readEnd == Reader::OPEN) {
      // Don't bother surfacing empty writes to the readers.
      if (!s.empty()) {
        if (data->reads.empty()) {
          data->writes.push(s);
        } else {
          read = data->reads.front();
          data->reads.pop();
        }
      }
      written = true;
    }
  }

  // NOTE: We set the promise outside the critical section to avoid
  // triggering callbacks that try to reacquire the lock.
  if (read.get() != NULL) {
    read->set(s);
  }

  return written;
}


bool Pipe::Writer::close()
{
  bool closed = false;
  queue<Owned<Promise<string>>> reads;

  synchronized (data->lock) {
    if (data->writeEnd == Writer::OPEN) {
      // Extract all the pending reads so we can complete them.
      std::swap(data->reads, reads);

      data->writeEnd = Writer::CLOSED;
      closed = true;
    }
  }

  // NOTE: We set the promises outside the critical section to avoid
  // triggering callbacks that try to reacquire the lock.
  while (!reads.empty()) {
    reads.front()->set(string("")); // End-of-file.
    reads.pop();
  }

  return closed;
}


bool Pipe::Writer::fail(const string& message)
{
  bool failed = false;
  queue<Owned<Promise<string>>> reads;

  synchronized (data->lock) {
    if (data->writeEnd == Writer::OPEN) {
      // Extract all the pending reads so we can fail them.
      std::swap(data->reads, reads);

      data->writeEnd = Writer::FAILED;
      data->failure = Failure(message);
      failed = true;
    }
  }

  // NOTE: We set the promises outside the critical section to avoid
  // triggering callbacks that try to reacquire the lock.
  while (!reads.empty()) {
    reads.front()->fail(message);
    reads.pop();
  }

  return failed;
}


Future<Nothing> Pipe::Writer::readerClosed() const
{
  return data->readerClosure.future();
}


OK::OK(JSON::Proxy&& value, const Option<std::string>& jsonp)
{
  type = BODY;

  status = "200 OK";

  std::ostringstream out;

  if (jsonp.isSome()) {
    out << jsonp.get() << "(";
  }

  out << std::move(value);

  if (jsonp.isSome()) {
    out << ");";
    headers["Content-Type"] = "text/javascript";
  } else {
    headers["Content-Type"] = "application/json";
  }

  body = out.str();
  headers["Content-Length"] = stringify(body.size());
}

namespace path {

Try<hashmap<string, string>> parse(const string& pattern, const string& path)
{
  // Split the pattern by '/' into keys.
  vector<string> keys = strings::tokenize(pattern, "/");

  // Split the path by '/' into segments.
  vector<string> segments = strings::tokenize(path, "/");

  hashmap<string, string> result;

  while (!segments.empty()) {
    if (keys.empty()) {
      return Error(
          "Not expecting suffix '" + strings::join("/", segments) + "'");
    }

    string key = keys.front();

    if (strings::startsWith(key, "{") &&
        strings::endsWith(key, "}")) {
      key = strings::remove(key, "{", strings::PREFIX);
      key = strings::remove(key, "}", strings::SUFFIX);
    } else if (key != segments.front()) {
      return Error("Expecting '" + key + "' not '" + segments.front() + "'");
    }

    result[key] = segments.front();

    keys.erase(keys.begin());
    segments.erase(segments.begin());
  }

  return result;
}

} // namespace path {


string encode(const string& s)
{
  ostringstream out;

  foreach (unsigned char c, s) {
    switch (c) {
      // Reserved characters.
      case '$':
      case '&':
      case '+':
      case ',':
      case '/':
      case ':':
      case ';':
      case '=':
      case '?':
      case '@':
      // Unsafe characters.
      case ' ':
      case '"':
      case '<':
      case '>':
      case '#':
      case '%':
      case '{':
      case '}':
      case '|':
      case '\\':
      case '^':
      case '~':
      case '[':
      case ']':
      case '`':
        // NOTE: The cast to unsigned int is needed.
        out << '%' << std::setfill('0') << std::setw(2) << std::hex
            << std::uppercase << (unsigned int) c;
        break;
      default:
        // ASCII control characters and non-ASCII characters.
        // NOTE: The cast to unsigned int is needed.
        if (c < 0x20 || c > 0x7F) {
          out << '%' << std::setfill('0') << std::setw(2) << std::hex
              << std::uppercase << (unsigned int) c;
        } else {
          out << c;
        }
        break;
    }
  }

  return out.str();
}


Try<string> decode(const string& s)
{
  ostringstream out;

  for (size_t i = 0; i < s.length(); ++i) {
    if (s[i] != '%') {
      out << (s[i] == '+' ? ' ' : s[i]);
      continue;
    }

    // We now expect two more characters: "% HEXDIG HEXDIG"
    if (i + 2 >= s.length() || !isxdigit(s[i+1]) || !isxdigit(s[i+2])) {
      return Error(
          "Malformed % escape in '" + s + "': '" + s.substr(i, 3) + "'");
    }

    // Convert from HEXDIG HEXDIG to char value.
    istringstream in(s.substr(i + 1, 2));
    unsigned long l;
    in >> std::hex >> l;
    if (l > UCHAR_MAX) {
      ABORT("Unexpected conversion from hex string: " + s.substr(i + 1, 2) +
            " to unsigned long: " + stringify(l));
    }
    out << static_cast<unsigned char>(l);

    i += 2;
  }

  return out.str();
}


namespace query {

Try<hashmap<std::string, std::string>> decode(const std::string& query)
{
  hashmap<std::string, std::string> result;

  const std::vector<std::string> tokens = strings::tokenize(query, ";&");
  foreach (const std::string& token, tokens) {
    const std::vector<std::string> pairs = strings::split(token, "=", 2);
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


ostream& operator<<(ostream& stream, const URL& url)
{
  stream << url.scheme << "://";

  if (url.domain.isSome()) {
    stream << url.domain.get();
  } else if (url.ip.isSome()) {
    stream << url.ip.get();
  }

  stream << ":" << url.port;

  stream << "/" << strings::remove(url.path, "/", strings::PREFIX);

  if (!url.query.empty()) {
    stream << "?" << query::encode(url.query);
  }

  if (url.fragment.isSome()) {
    stream << "#" << url.fragment.get();
  }

  return stream;
}


namespace internal {

// Forward declarations.
Future<string> _convert(
    Pipe::Reader reader,
    const std::shared_ptr<string>& buffer,
    const string& read);
Response __convert(
    const Response& pipeResponse,
    const string& body);


// Returns a 'BODY' response once the body of the provided
// 'PIPE' response can be read completely.
Future<Response> convert(const Response& pipeResponse)
{
  std::shared_ptr<string> buffer(new string());

  CHECK_EQ(Response::PIPE, pipeResponse.type);
  CHECK_SOME(pipeResponse.reader);

  Pipe::Reader reader = pipeResponse.reader.get();

  return reader.read()
    .then(lambda::bind(&_convert, reader, buffer, lambda::_1))
    .then(lambda::bind(&__convert, pipeResponse, lambda::_1));
}


Future<string> _convert(
    Pipe::Reader reader,
    const std::shared_ptr<string>& buffer,
    const string& read)
{
  if (read.empty()) { // EOF.
    return *buffer;
  }

  buffer->append(read);

  return reader.read()
    .then(lambda::bind(&_convert, reader, buffer, lambda::_1));
}


Response __convert(const Response& pipeResponse, const string& body)
{
  Response bodyResponse = pipeResponse;
  bodyResponse.type = Response::BODY;
  bodyResponse.body = body;
  bodyResponse.reader = None(); // Remove the reader.
  return bodyResponse;
}


// Forward declaration.
void _decode(
    Socket socket,
    Owned<StreamingResponseDecoder> decoder,
    const Future<string>& data);


Future<Response> decode(
    Socket socket,
    Owned<StreamingResponseDecoder> decoder,
    const string& data)
{
  deque<Response*> responses = decoder->decode(data.c_str(), data.length());

  if (decoder->failed() || responses.size() > 1) {
    foreach (Response* response, responses) {
      delete response;
    }
    return Failure(string("Failed to decode HTTP response") +
        (responses.size() > 1 ? ": more than one response received" : ""));
  }

  if (responses.empty()) {
    // Keep reading until the headers are complete.
    return socket.recv(None())
      .then(lambda::bind(&decode, socket, decoder, lambda::_1));
  }

  // Keep feeding data to the decoder until EOF or a 'recv' failure.
  if (!data.empty()) {
    socket.recv(None())
      .onAny(lambda::bind(&_decode, socket, decoder, lambda::_1));
  }

  Response response = *responses[0];
  delete responses[0];
  return response;
}


void _decode(
    Socket socket,
    Owned<StreamingResponseDecoder> decoder,
    const Future<string>& data)
{
  deque<Response*> responses;

  if (!data.isReady()) {
    // Let the decoder process EOF if a failure
    // or discard is encountered.
    responses = decoder->decode("", 0);
  } else {
    responses = decoder->decode(data.get().c_str(), data.get().length());
  }

  // We're not expecting more responses to arrive on this socket!
  if (!responses.empty() || decoder->failed()) {
    VLOG(1) << "Failed to decode HTTP response: "
            << (responses.size() > 1
                ? ": more than one response received"
                : "");

    foreach (Response* response, responses) {
      delete response;
    }

    return;
  }

  // Keep reading if the socket has more data.
  if (data.isReady() && !data.get().empty()) {
    socket.recv(None())
      .onAny(lambda::bind(&_decode, socket, decoder, lambda::_1));
  }
}


// Forward declaration.
Future<Response> _request(
    Socket socket,
    const Address& address,
    const URL& url,
    const string& method,
    bool streamingResponse,
    const Option<hashmap<string, string>>& _headers,
    const Option<string>& body,
    const Option<string>& contentType);


Future<Response> request(
    const URL& url,
    const string& method,
    bool streamedResponse,
    const Option<hashmap<string, string>>& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  auto create = [&url]() -> Try<Socket> {
    if (url.scheme == "http") {
      return Socket::create(Socket::POLL);
    }

#ifdef USE_SSL_SOCKET
    if (url.scheme == "https") {
      return Socket::create(Socket::SSL);
    }
#endif

    return Error("Unsupported URL scheme");
  }();

  if (create.isError()) {
    return Failure("Failed to create socket: " + create.error());
  }

  Socket socket = create.get();

  Address address;

  if (url.ip.isSome()) {
    address.ip = url.ip.get();
  } else if (url.domain.isNone()) {
    return Failure("Missing URL domain or IP");
  } else {
    Try<net::IP> ip = net::getIP(url.domain.get(), AF_INET);

    if (ip.isError()) {
      return Failure("Failed to determine IP of domain '" +
                     url.domain.get() + "': " + ip.error());
    }

    address.ip = ip.get();
  }

  address.port = url.port;

  return socket.connect(address)
    .then(lambda::bind(&_request,
                       socket,
                       address,
                       url,
                       method,
                       streamedResponse,
                       headers,
                       body,
                       contentType));
}


Future<Response> _request(
    Socket socket,
    const Address& address,
    const URL& url,
    const string& method,
    bool streamedResponse,
    const Option<hashmap<string, string>>& _headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  std::ostringstream out;

  out << method << " /" << strings::remove(url.path, "/", strings::PREFIX);

  if (!url.query.empty()) {
    // Convert the query to a string that we join via '=' and '&'.
    vector<string> query;

    foreachpair (const string& key, const string& value, url.query) {
      query.push_back(key + "=" + value);
    }

    out << "?" << strings::join("&", query);
  }

  if (url.fragment.isSome()) {
    out << "#" << url.fragment.get();
  }

  out << " HTTP/1.1\r\n";

  // Set up the headers as necessary.
  hashmap<string, string> headers;

  if (_headers.isSome()) {
    headers = _headers.get();
  }

  // Need to specify the 'Host' header.
  if (url.domain.isSome()) {
    // Use ONLY domain for standard ports.
    if (url.port == 80 || url.port == 443) {
      headers["Host"] = url.domain.get();
    } else {
      // Add port for non-standard ports.
      headers["Host"] = url.domain.get() + ":" + stringify(url.port);
    }
  } else {
    headers["Host"] = stringify(address);
  }

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

  // TODO(bmahler): Use a 'Request' and a 'RequestEncoder' here!
  // Currently this does not handle 'gzip' content encoding,
  // unless the caller manually compresses the 'body'. For
  // streaming requests we must wipe 'gzip' as an acceptable
  // encoding as we don't currently have streaming gzip utilities
  // to support decoding a streaming gzip response!

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

  Owned<StreamingResponseDecoder> decoder(new StreamingResponseDecoder());

  Future<Response> pipeResponse = socket.send(out.str())
    .then(lambda::function<Future<string>(void)>(
              lambda::bind(recv, socket, None())))
    .then(lambda::bind(&internal::decode, socket, decoder, lambda::_1));

  if (streamedResponse) {
    return pipeResponse;
  } else {
    return pipeResponse
      .then(lambda::bind(&internal::convert, lambda::_1));
  }
}

} // namespace internal {


Future<Response> get(
    const URL& url,
    const Option<hashmap<string, string>>& headers)
{
  return internal::request(url, "GET", false, headers, None(), None());
}


Future<Response> get(
    const UPID& upid,
    const Option<string>& path,
    const Option<string>& query,
    const Option<hashmap<string, string>>& headers)
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
    const URL& url,
    const Option<hashmap<string, string>>& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  if (body.isNone() && contentType.isSome()) {
    return Failure("Attempted to do a POST with a Content-Type but no body");
  }

  return internal::request(url, "POST", false, headers, body, contentType);
}


Future<Response> post(
    const UPID& upid,
    const Option<string>& path,
    const Option<hashmap<string, string>>& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  URL url("http", net::IP(upid.address.ip), upid.address.port, upid.id);

  if (path.isSome()) {
    // TODO(benh): Get 'query' and/or 'fragment' out of 'path'.
    url.path = strings::join("/", url.path, path.get());
  }

  return post(url, headers, body, contentType);
}


Future<Response> requestDelete(
    const URL& url,
    const Option<hashmap<string, string>>& headers)
{
  return internal::request(url, "DELETE", false, headers, None(), None());
}


Future<Response> requestDelete(
    const UPID& upid,
    const Option<string>& path,
    const Option<hashmap<string, string>>& headers)
{
  URL url("http", net::IP(upid.address.ip), upid.address.port, upid.id);

  if (path.isSome()) {
    // TODO(joerg84): Handle 'query' and/or 'fragment' in 'path'.
    // This could mean that we either remove these or even fail
    // (see MESOS-3163).
    url.path = strings::join("/", url.path, path.get());
  }

  return requestDelete(url, headers);
}


namespace streaming {

Future<Response> get(
    const URL& url,
    const Option<hashmap<string, string>>& headers)
{
  return internal::request(url, "GET", true, headers, None(), None());
}


Future<Response> get(
    const UPID& upid,
    const Option<string>& path,
    const Option<string>& query,
    const Option<hashmap<string, string>>& headers)
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

  return streaming::get(url, headers);
}


Future<Response> post(
    const URL& url,
    const Option<hashmap<string, string>>& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  if (body.isNone() && contentType.isSome()) {
    return Failure("Attempted to do a POST with a Content-Type but no body");
  }

  return internal::request(url, "POST", true, headers, body, contentType);
}


Future<Response> post(
    const UPID& upid,
    const Option<string>& path,
    const Option<hashmap<string, string>>& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  URL url("http", net::IP(upid.address.ip), upid.address.port, upid.id);

  if (path.isSome()) {
    // TODO(benh): Get 'query' and/or 'fragment' out of 'path'.
    url.path = strings::join("/", url.path, path.get());
  }

  return streaming::post(url, headers, body, contentType);
}

} // namespace streaming {

} // namespace http {
} // namespace process {
