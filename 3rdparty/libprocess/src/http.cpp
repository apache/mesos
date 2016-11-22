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

#ifndef __WINDOWS__
#include <arpa/inet.h>
#endif // __WINDOWS__

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
#include <tuple>
#include <vector>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
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
using std::tuple;
using std::vector;

using process::http::Request;
using process::http::Response;

using process::network::Address;
using process::network::Socket;

namespace process {
namespace http {


hashmap<uint16_t, string>* statuses = new hashmap<uint16_t, string> {
  {100, "100 Continue"},
  {101, "101 Switching Protocols"},
  {200, "200 OK"},
  {201, "201 Created"},
  {202, "202 Accepted"},
  {203, "203 Non-Authoritative Information"},
  {204, "204 No Content"},
  {205, "205 Reset Content"},
  {206, "206 Partial Content"},
  {300, "300 Multiple Choices"},
  {301, "301 Moved Permanently"},
  {302, "302 Found"},
  {303, "303 See Other"},
  {304, "304 Not Modified"},
  {305, "305 Use Proxy"},
  {307, "307 Temporary Redirect"},
  {400, "400 Bad Request"},
  {401, "401 Unauthorized"},
  {402, "402 Payment Required"},
  {403, "403 Forbidden"},
  {404, "404 Not Found"},
  {405, "405 Method Not Allowed"},
  {406, "406 Not Acceptable"},
  {407, "407 Proxy Authentication Required"},
  {408, "408 Request Time-out"},
  {409, "409 Conflict"},
  {410, "410 Gone"},
  {411, "411 Length Required"},
  {412, "412 Precondition Failed"},
  {413, "413 Request Entity Too Large"},
  {414, "414 Request-URI Too Large"},
  {415, "415 Unsupported Media Type"},
  {416, "416 Requested range not satisfiable"},
  {417, "417 Expectation Failed"},
  {500, "500 Internal Server Error"},
  {501, "501 Not Implemented"},
  {502, "502 Bad Gateway"},
  {503, "503 Service Unavailable"},
  {504, "504 Gateway Time-out"},
  {505, "505 HTTP Version not supported"}
};


const uint16_t Status::CONTINUE = 100;
const uint16_t Status::SWITCHING_PROTOCOLS = 101;
const uint16_t Status::OK = 200;
const uint16_t Status::CREATED = 201;
const uint16_t Status::ACCEPTED = 202;
const uint16_t Status::NON_AUTHORITATIVE_INFORMATION = 203;
const uint16_t Status::NO_CONTENT = 204;
const uint16_t Status::RESET_CONTENT = 205;
const uint16_t Status::PARTIAL_CONTENT = 206;
const uint16_t Status::MULTIPLE_CHOICES = 300;
const uint16_t Status::MOVED_PERMANENTLY = 301;
const uint16_t Status::FOUND = 302;
const uint16_t Status::SEE_OTHER = 303;
const uint16_t Status::NOT_MODIFIED = 304;
const uint16_t Status::USE_PROXY = 305;
const uint16_t Status::TEMPORARY_REDIRECT = 307;
const uint16_t Status::BAD_REQUEST = 400;
const uint16_t Status::UNAUTHORIZED = 401;
const uint16_t Status::PAYMENT_REQUIRED = 402;
const uint16_t Status::FORBIDDEN = 403;
const uint16_t Status::NOT_FOUND = 404;
const uint16_t Status::METHOD_NOT_ALLOWED = 405;
const uint16_t Status::NOT_ACCEPTABLE = 406;
const uint16_t Status::PROXY_AUTHENTICATION_REQUIRED = 407;
const uint16_t Status::REQUEST_TIMEOUT = 408;
const uint16_t Status::CONFLICT = 409;
const uint16_t Status::GONE = 410;
const uint16_t Status::LENGTH_REQUIRED = 411;
const uint16_t Status::PRECONDITION_FAILED = 412;
const uint16_t Status::REQUEST_ENTITY_TOO_LARGE = 413;
const uint16_t Status::REQUEST_URI_TOO_LARGE = 414;
const uint16_t Status::UNSUPPORTED_MEDIA_TYPE = 415;
const uint16_t Status::REQUESTED_RANGE_NOT_SATISFIABLE = 416;
const uint16_t Status::EXPECTATION_FAILED = 417;
const uint16_t Status::INTERNAL_SERVER_ERROR = 500;
const uint16_t Status::NOT_IMPLEMENTED = 501;
const uint16_t Status::BAD_GATEWAY = 502;
const uint16_t Status::SERVICE_UNAVAILABLE = 503;
const uint16_t Status::GATEWAY_TIMEOUT = 504;
const uint16_t Status::HTTP_VERSION_NOT_SUPPORTED = 505;


string Status::string(uint16_t code)
{
  return http::statuses->get(code)
    .getOrElse(stringify(code));
}


// Returns the default port for a given URL scheme.
static Option<uint16_t> defaultPort(const string& scheme)
{
  // TODO(tnachen): Make default port a lookup table.
  if (scheme == "http") {
    return 80;
  } else if (scheme == "https") {
    return 443;
  }

  return None();
}


Try<URL> URL::parse(const string& urlString)
{
  // TODO(tnachen): Consider using C++11 regex support instead.

  size_t schemePos = urlString.find_first_of("://");
  if (schemePos == string::npos) {
    return Error("Missing scheme in url string");
  }

  const string scheme = strings::lower(urlString.substr(0, schemePos));
  const string urlPath = urlString.substr(schemePos + 3);

  size_t pathPos = urlPath.find_first_of("/");
  if (pathPos == 0) {
    return Error("Host not found in url");
  }

  // If path is specified in the URL, try to capture the host and path
  // seperately.
  string host = urlPath;
  string path = "/";
  if (pathPos != string::npos) {
    host = host.substr(0, pathPos);
    path = urlPath.substr(pathPos);
  }

  if (host.empty()) {
    return Error("Host not found in url");
  }

  const vector<string> tokens = strings::tokenize(host, ":");

  if (tokens[0].empty()) {
    return Error("Host not found in url");
  }

  if (tokens.size() > 2) {
    return Error("Found multiple ports in url");
  }

  Option<uint16_t> port;
  if (tokens.size() == 2) {
    Try<uint16_t> numifyPort = numify<uint16_t>(tokens[1]);
    if (numifyPort.isError()) {
      return Error("Failed to parse port: " + numifyPort.error());
    }

    port = numifyPort.get();
  } else {
    // Attempt to resolve the port based on the URL scheme.
    port = defaultPort(scheme);
  }

  if (port.isNone()) {
    return Error("Unable to determine port from url");
  }

  // TODO(tnachen): Support parsing query and fragment.

  return URL(scheme, tokens[0], port.get(), path);
}


bool URL::isAbsolute() const
{
  return scheme.isSome();
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


Future<string> Pipe::Reader::readAll()
{
  std::shared_ptr<string> buffer(new string());

  return _readAll(*this, buffer);
}


Future<string> Pipe::Reader::_readAll(
    Pipe::Reader reader,
    const std::shared_ptr<string>& buffer)
{
  return reader.read()
    .then([reader, buffer](const string& read) -> Future<string> {
      if (read.empty()) { // EOF.
        return std::move(*buffer);
      }

      buffer->append(read);

      return _readAll(reader, buffer);
    });
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
    } else {
      // This future is only satisifed when the reader is closed before
      // the write-end of the pipe. In other cases, discard the promise
      // in order to clear any associated callbacks.
      data->readerClosure.discard();
    }
  }

  return closed;
}


bool Pipe::Writer::write(string s)
{
  bool written = false;
  Owned<Promise<string>> read;

  synchronized (data->lock) {
    // Ignore writes if either end of the pipe is closed or failed!
    if (data->writeEnd == Writer::OPEN && data->readEnd == Reader::OPEN) {
      // Don't bother surfacing empty writes to the readers.
      if (!s.empty()) {
        if (data->reads.empty()) {
          data->writes.push(std::move(s));
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
  if (read.get() != nullptr) {
    read->set(std::move(s));
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


OK::OK(const JSON::Value& value, const Option<string>& jsonp)
  : Response(Status::OK)
{
  type = BODY;

  std::ostringstream out;

  if (jsonp.isSome()) {
    out << jsonp.get() << "(";
  }

  out << value;

  if (jsonp.isSome()) {
    out << ");";
    headers["Content-Type"] = "text/javascript";
  } else {
    headers["Content-Type"] = "application/json";
  }

  headers["Content-Length"] = stringify(out.str().size());
  body = out.str().data();
}


OK::OK(JSON::Proxy&& value, const Option<string>& jsonp)
  : Response(Status::OK)
{
  type = BODY;

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


Try<vector<Response>> decodeResponses(const string& s)
{
  ResponseDecoder decoder;

  deque<http::Response*> responses = decoder.decode(s.data(), s.length());

  if (decoder.failed()) {
    foreach (Response* response, responses) {
      delete response;
    }

    return Error("Decoding failed");
  }

  if (responses.empty()) {
    return Error("No response decoded");
  }

  vector<Response> result;

  foreach (Response* response, responses) {
    result.push_back(*response);
    delete response;
  }

  return result;
}


namespace query {

Try<hashmap<string, string>> decode(const string& query)
{
  hashmap<string, string> result;

  const vector<string> tokens = strings::tokenize(query, ";&");
  foreach (const string& token, tokens) {
    const vector<string> pairs = strings::split(token, "=", 2);
    if (pairs.size() == 0) {
      continue;
    }

    Try<string> key = http::decode(pairs[0]);
    if (key.isError()) {
      return Error(key.error());
    }

    if (pairs.size() == 2) {
      Try<string> value = http::decode(pairs[1]);
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


string encode(const hashmap<string, string>& query)
{
  string output;

  foreachpair (const string& key, const string& value, query) {
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
  if (url.scheme.isSome()) {
    stream << url.scheme.get() << "://";
  }

  if (url.domain.isSome()) {
    stream << url.domain.get();
  } else if (url.ip.isSome()) {
    stream << url.ip.get();
  }

  if (url.port.isSome()) {
    stream << ":" << url.port.get();
  }

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

void _encode(Pipe::Reader reader, Pipe::Writer writer); // Forward declaration.


// Encodes the request by writing into a pipe, the caller can
// read the encoded data from the returned read end of the pipe.
// A pipe is used since the request body can be a pipe and must
// be read asynchronously.
Pipe::Reader encode(const Request& request)
{
  // TODO(bmahler): Replace this with a RequestEncoder.
  std::ostringstream out;

  out << request.method
      << " /" << strings::remove(request.url.path, "/", strings::PREFIX);

  if (!request.url.query.empty()) {
    // Convert the query to a string that we join via '=' and '&'.
    vector<string> query;

    foreachpair (const string& key, const string& value, request.url.query) {
      query.push_back(key + "=" + value);
    }

    out << "?" << strings::join("&", query);
  }

  if (request.url.fragment.isSome()) {
    out << "#" << request.url.fragment.get();
  }

  out << " HTTP/1.1\r\n";

  // Overwrite headers as necessary.
  Headers headers = request.headers;

  // Need to specify the 'Host' header.
  CHECK(request.url.domain.isSome() || request.url.ip.isSome());

  if (request.url.domain.isSome()) {
    headers["Host"] = request.url.domain.get();
  } else if (request.url.ip.isSome()) {
    headers["Host"] = stringify(request.url.ip.get());
  }

  // Add port for non-standard ports.
  if (request.url.port.isSome() &&
      request.url.port != 80 &&
      request.url.port != 443) {
    headers["Host"] += ":" + stringify(request.url.port.get());
  }

  if (!request.keepAlive) {
    // Tell the server to close the connection when it's done.
    headers["Connection"] = "close";
  }

  if (request.type == Request::PIPE) {
    CHECK(!headers.contains("Content-Length"));

    // Make sure the Transfer-Encoding header is set correctly
    // for PIPE requests.
    headers["Transfer-Encoding"] = "chunked";
  } else {
    CHECK_EQ(Request::BODY, request.type);

    // Make sure the Content-Length header is set correctly.
    headers["Content-Length"] = stringify(request.body.length());
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

  Pipe pipe;
  Pipe::Reader reader = pipe.reader();
  Pipe::Writer writer = pipe.writer();

  // Write the head of the request.
  writer.write(out.str());

  switch (request.type) {
    case Request::BODY:
      writer.write(request.body);
      writer.close();
      break;
    case Request::PIPE:
      CHECK_SOME(request.reader);
      CHECK(request.body.empty());
      _encode(request.reader.get(), writer);
      break;
  }

  return reader;
}


void _encode(Pipe::Reader reader, Pipe::Writer writer)
{
  reader.read()
    .onAny([reader, writer](const Future<string>& chunk) mutable {
      if (!chunk.isReady()) {
        writer.fail(chunk.isFailed() ? chunk.failure() : "discarded");
        return;
      }

      if (chunk->empty()) {
        // EOF case.
        writer.write("0\r\n\r\n");
        writer.close();
        return;
      }

      std::ostringstream out;
      out << std::hex << chunk->size() << "\r\n";
      out << chunk.get();
      out << "\r\n";

      writer.write(out.str());
      _encode(reader, writer);
    });
}


// Returns a 'BODY' response once the body of the provided
// 'PIPE' response can be read completely.
Future<Response> convert(const Response& pipeResponse)
{
  CHECK_EQ(Response::PIPE, pipeResponse.type);
  CHECK_SOME(pipeResponse.reader);

  Pipe::Reader reader = pipeResponse.reader.get();

  return reader.readAll()
    .then([pipeResponse](const string& body) {
      Response bodyResponse = pipeResponse;
      bodyResponse.type = Response::BODY;
      bodyResponse.body = body;
      bodyResponse.reader = None(); // Remove the reader.
      return bodyResponse;
    });
}


class ConnectionProcess : public Process<ConnectionProcess>
{
public:
  ConnectionProcess(const Socket& _socket)
    : ProcessBase(ID::generate("__http_connection__")),
      socket(_socket),
      sendChain(Nothing()),
      close(false) {}

  Future<Response> send(const Request& request, bool streamedResponse)
  {
    if (!disconnection.future().isPending()) {
      return Failure("Disconnected");
    }

    if (close) {
      return Failure("Cannot pipeline after 'Connection: close'");
    }

    if (request.type == Request::PIPE) {
      if (request.reader.isNone()) {
        return Failure("Request reader must be set for PIPE request");
      }

      if (!request.body.empty()) {
        return Failure("Request body must be empty for PIPE request");
      }

      Option<string> contentLength = request.headers.get("Content-Length");
      if (request.headers.contains("Content-Length")) {
        return Failure("'Content-Length' cannot be set for PIPE request");
      }
    }

    if (!request.keepAlive) {
      close = true;
    }

    // We must chain the calls to Socket::send as it
    // otherwise interleaves data across calls.
    Socket socket_ = socket;

    sendChain = sendChain
      .then([socket_, request]() {
        return _send(socket_, encode(request));
      });

    // If we can't write to the socket, disconnect.
    sendChain
      .onFailed(defer(self(), [this](const string& failure) {
        disconnect(failure);
      }));

    Promise<Response> promise;
    Future<Response> response = promise.future();

    pipeline.push(std::make_tuple(streamedResponse, std::move(promise)));

    return response;
  }

  Future<Nothing> disconnect(const Option<string>& message = None())
  {
    Try<Nothing> shutdown = socket.shutdown();

    disconnection.set(Nothing());

    // If a response is still streaming, we send EOF to
    // the decoder in order to fail the pipe reader.
    if (decoder.writingBody()) {
      decoder.decode("", 0);
    }

    // Fail any remaining pipelined responses.
    while (!pipeline.empty()) {
      std::get<1>(pipeline.front()).fail(
          message.isSome() ? message.get() : "Disconnected");
      pipeline.pop();
    }

    return shutdown;
  }

  Future<Nothing> disconnected()
  {
    return disconnection.future();
  }

protected:
  virtual void initialize()
  {
    // Start the read loop on the socket. We read independently
    // of the requests being sent in order to detect socket
    // closure at any time.
    read();
  }

  virtual void finalize()
  {
    disconnect("Connection object was destructed");
  }

private:
  static Future<Nothing> _send(Socket socket, Pipe::Reader reader)
  {
    return reader.read()
      .then([socket, reader](const string& data) mutable -> Future<Nothing> {
        if (data.empty()) {
          return Nothing(); // EOF.
        }

        return socket.send(data)
          .then(lambda::bind(_send, socket, reader));
      });
  }

  void read()
  {
    socket.recv()
      .onAny(defer(self(), &Self::_read, lambda::_1));
  }

  void _read(const Future<string>& data)
  {
    deque<Response*> responses;

    if (!data.isReady() || data->empty()) {
      // Process EOF. Also send EOF to the decoder if a failure
      // or discard is encountered.
      responses = decoder.decode("", 0);
    } else {
      // We should only receive data if we're expecting a response
      // in the pipeline, or if a response body is still streaming.
      if (pipeline.empty() && !decoder.writingBody()) {
        disconnect("Received data when none is expected");
        return;
      }

      responses = decoder.decode(data->data(), data->length());
    }

    // Process any decoded responses.
    while (!responses.empty()) {
      // We do not expect any responses when the pipeline is empty.
      // Note that this may occur when a 'Connection: close' header
      // prematurely terminates the pipeline.
      if (pipeline.empty()) {
        while (!responses.empty()) {
          delete responses.front();
          responses.pop_front();
        }

        disconnect("Received response without a request");
        return;
      }

      Response* response = responses.front();
      responses.pop_front();

      tuple<bool, Promise<Response>> t = std::move(pipeline.front());
      pipeline.pop();

      bool streamedResponse = std::get<0>(t);
      Promise<Response> promise = std::move(std::get<1>(t));

      if (streamedResponse) {
        promise.set(*response);
      } else {
        // If the response should not be streamed, we convert
        // the PIPE response into a BODY response.
        promise.associate(convert(*response));
      }

      if (response->headers.contains("Connection") &&
          response->headers.at("Connection") == "close") {
        // This is the last response the server will send!
        close = true;

        // Fail the remainder of the pipeline.
        while (!pipeline.empty()) {
          std::get<1>(pipeline.front()).fail(
              "Received 'Connection: close' from the server");
          pipeline.pop();
        }
      }

      delete response;
    }

    // We keep reading and feeding data to the decoder until
    // EOF or a failure is encountered.
    if (!data.isReady()) {
      disconnect(data.isFailed() ? data.failure() : "discarded");
      return;
    } else if (data->empty()) {
      disconnect(); // EOF.
      return;
    } else if (decoder.failed()) {
      disconnect("Failed to decode response");
      return;
    }

    // Close the connection if a 'Connection: close' header
    // was found and we're done reading the last response.
    if (close && pipeline.empty() && !decoder.writingBody()) {
      disconnect();
      return;
    }

    read();
  }

  Socket socket;
  StreamingResponseDecoder decoder;
  Future<Nothing> sendChain;
  Promise<Nothing> disconnection;

  // For each response in the pipeline, we store a bool for
  // whether the caller wants the response to be streamed.
  queue<tuple<bool, Promise<Response>>> pipeline;

  // Whether the connection should be closed upon the
  // completion of the last outstanding response.
  bool close;
};

} // namespace internal {


struct Connection::Data
{
  // We spawn `ConnectionProcess` as a managed process to guarantee
  // that it does not wait on itself (this would cause a deadlock!).
  // See MESOS-4658 for details.
  //
  // TODO(bmahler): This surfaces a general pattern that we
  // should enforce: have libprocess manage a Process when
  // it cannot be guaranteed that the Process will be waited
  // on within a different execution context. More generally,
  // we should be passing Process ownership to libprocess to
  // ensure all interaction with a Process occurs through a PID.
  Data(const Socket& s)
    : process(spawn(new internal::ConnectionProcess(s), true)) {}

  ~Data()
  {
    // Note that we pass 'false' here to avoid injecting the
    // termination event at the front of the queue. This is
    // to ensure we don't drop any queued request dispatches
    // which would leave the caller with a future stuck in
    // a pending state.
    terminate(process, false);
  }

  PID<internal::ConnectionProcess> process;
};


Connection::Connection(const Socket& s)
  : data(std::make_shared<Connection::Data>(s)) {}


Future<Response> Connection::send(
    const http::Request& request,
    bool streamedResponse)
{
  return dispatch(
      data->process,
      &internal::ConnectionProcess::send,
      request,
      streamedResponse);
}


Future<Nothing> Connection::disconnect()
{
  return dispatch(
      data->process,
      &internal::ConnectionProcess::disconnect,
      None());
}


Future<Nothing> Connection::disconnected()
{
  return dispatch(
      data->process,
      &internal::ConnectionProcess::disconnected);
}


Future<Connection> connect(const URL& url)
{
  // TODO(bmahler): Move address resolution into the URL class?
  Address address;

  if (url.ip.isNone() && url.domain.isNone()) {
    return Failure("Expected URL.ip or URL.domain to be set");
  }

  if (url.ip.isSome()) {
    address.ip = url.ip.get();
  } else {
    Try<net::IP> ip = net::getIP(url.domain.get(), AF_INET);

    if (ip.isError()) {
      return Failure("Failed to determine IP of domain '" +
                     url.domain.get() + "': " + ip.error());
    }

    address.ip = ip.get();
  }

  if (url.port.isNone()) {
    return Failure("Expecting url.port to be set");
  }

  address.port = url.port.get();

  Try<Socket> socket = [&url]() -> Try<Socket> {
    // Default to 'http' if no scheme was specified.
    if (url.scheme.isNone() || url.scheme == string("http")) {
      return Socket::create(Socket::POLL);
    }

    if (url.scheme == string("https")) {
#ifdef USE_SSL_SOCKET
      return Socket::create(Socket::SSL);
#else
      return Error("'https' scheme requires SSL enabled");
#endif
    }

    return Error("Unsupported URL scheme");
  }();

  if (socket.isError()) {
    return Failure("Failed to create socket: " + socket.error());
  }

  return socket->connect(address)
    .then([socket]() {
      return Connection(socket.get());
    });
}


Request createRequest(
    const URL& url,
    const string& method,
    const Option<Headers>& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  Request request;
  request.method = method;
  request.url = url;
  request.keepAlive = false;

  if (headers.isSome()) {
    request.headers = headers.get();
  }

  if (body.isSome()) {
    request.body = body.get();
  }

  if (contentType.isSome()) {
    request.headers["Content-Type"] = contentType.get();
  }

  return request;
}


Request createRequest(
  const UPID& upid,
  const string& method,
  bool enableSSL,
  const Option<string>& path,
  const Option<Headers>& headers,
  const Option<string>& body,
  const Option<string>& contentType)
{
  string scheme = enableSSL ? "https" : "http";
  URL url(scheme, net::IP(upid.address.ip), upid.address.port, upid.id);

  if (path.isSome()) {
    url.path = strings::join("/", url.path, path.get());
  }

  return createRequest(url, method, headers, body, contentType);
}


Future<Response> request(const Request& request, bool streamedResponse)
{
  // We rely on the connection closing after the response.
  CHECK(!request.keepAlive);

  return http::connect(request.url)
    .then([=](Connection connection) {
      Future<Response> response = connection.send(request, streamedResponse);

      // This is a non Keep-Alive request which means the connection
      // will be closed when the response is received. Since the
      // 'Connection' is reference-counted, we must maintain a copy
      // until the disconnection occurs.
      connection.disconnected()
        .onAny([connection]() {});

      return response;
    });
}


Future<Response> get(
    const URL& url,
    const Option<Headers>& headers)
{
  Request _request;
  _request.method = "GET";
  _request.url = url;
  _request.keepAlive = false;

  if (headers.isSome()) {
    _request.headers = headers.get();
  }

  return request(_request, false);
}


Future<Response> get(
    const UPID& upid,
    const Option<string>& path,
    const Option<string>& query,
    const Option<Headers>& headers)
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
    const Option<Headers>& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  if (body.isNone() && contentType.isSome()) {
    return Failure("Attempted to do a POST with a Content-Type but no body");
  }

  Request _request;
  _request.method = "POST";
  _request.url = url;
  _request.keepAlive = false;

  if (headers.isSome()) {
    _request.headers = headers.get();
  }

  if (body.isSome()) {
    _request.body = body.get();
  }

  if (contentType.isSome()) {
    _request.headers["Content-Type"] = contentType.get();
  }

  return request(_request, false);
}


Future<Response> post(
    const UPID& upid,
    const Option<string>& path,
    const Option<Headers>& headers,
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
    const Option<Headers>& headers)
{
  Request _request;
  _request.method = "DELETE";
  _request.url = url;
  _request.keepAlive = false;

  if (headers.isSome()) {
    _request.headers = headers.get();
  }

  return request(_request, false);
}


Future<Response> requestDelete(
    const UPID& upid,
    const Option<string>& path,
    const Option<Headers>& headers)
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
    const Option<Headers>& headers)
{
  Request _request;
  _request.method = "GET";
  _request.url = url;
  _request.keepAlive = false;

  if (headers.isSome()) {
    _request.headers = headers.get();
  }

  return request(_request, true);
}


Future<Response> get(
    const UPID& upid,
    const Option<string>& path,
    const Option<string>& query,
    const Option<Headers>& headers)
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
    const Option<Headers>& headers,
    const Option<string>& body,
    const Option<string>& contentType)
{
  if (body.isNone() && contentType.isSome()) {
    return Failure("Attempted to do a POST with a Content-Type but no body");
  }

  Request _request;
  _request.method = "POST";
  _request.url = url;
  _request.keepAlive = false;

  if (body.isSome()) {
    _request.body = body.get();
  }

  if (headers.isSome()) {
    _request.headers = headers.get();
  }

  if (contentType.isSome()) {
    _request.headers["Content-Type"] = contentType.get();
  }

  return request(_request, true);
}


Future<Response> post(
    const UPID& upid,
    const Option<string>& path,
    const Option<Headers>& headers,
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
