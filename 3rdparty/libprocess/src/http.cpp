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

#include <process/after.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/id.hpp>
#include <process/io.hpp>
#include <process/loop.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/queue.hpp>
#include <process/socket.hpp>
#include <process/state_machine.hpp>

#include <process/ssl/tls_config.hpp>

#include <stout/error.hpp>
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
#include <stout/unreachable.hpp>

#include "decoder.hpp"
#include "encoder.hpp"

using std::deque;
using std::istringstream;
using std::list;
using std::map;
using std::ostream;
using std::ostringstream;
using std::queue;
using std::string;
using std::tuple;
using std::vector;

using process::http::Request;
using process::http::Response;

namespace inet4 = process::network::inet4;

using process::network::inet::Address;
using process::network::inet::Socket;

using process::network::internal::SocketImpl;

namespace process {
namespace http {

struct StatusDescription {
  uint16_t code;
  const char* description;
};


// Status code reason strings, from the HTTP1.1 RFC:
// http://www.w3.org/Protocols/rfc2616/rfc2616-sec6.html
StatusDescription statuses[] = {
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


// Since the status codes are stored in increasing order, we could also
// use std::lower_bound to do the lookup with logarithmic complexity.
// However, according to some cursory research, on most CPUs this will
// be slower until the array size is around 100 elemnts.
//
// [1]: https://schani.wordpress.com/2010/04/30/linear-vs-binary-search/
// [2]: https://stackoverflow.com/questions/1275665/at-which-n-does-binary-search-become-faster-than-linear-search-on-a-modern-cpu

string Status::string(uint16_t code)
{
  auto value = std::find_if(
      std::begin(statuses),
      std::end(statuses),
      [code](const StatusDescription& sd) { return sd.code == code; });

  if (value != std::end(statuses)) {
    return value->description;
  }

  // Fallback for unknown status codes.
  return stringify(code);
}


bool isValidStatus(uint16_t code)
{
  return std::end(statuses) != std::find_if(
      std::begin(statuses),
      std::end(statuses),
      [code](const StatusDescription& sd) { return sd.code == code; });
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

  size_t schemePos = urlString.find("://");
  if (schemePos == string::npos) {
    return Error("Missing scheme in url string");
  }

  const string scheme = strings::lower(urlString.substr(0, schemePos));
  const string urlPath = urlString.substr(schemePos + 3);

  size_t pathPos = urlPath.find_first_of('/');
  if (pathPos == 0) {
    return Error("Host not found in url");
  }

  // If path is specified in the URL, try to capture the host and path
  // separately.
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

  if (accept.isNone() || accept->empty()) {
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
  return _acceptsMediaType(headers.get("Accept"), mediaType);
}


bool Request::acceptsMediaType(
    const string& name,
    const string& mediaType) const
{
  return _acceptsMediaType(headers.get(name), mediaType);
}


bool Request::_acceptsMediaType(
    Option<string> name,
    const string& mediaType) const
{
  vector<string> mediaTypes = strings::tokenize(mediaType, "/");

  if (mediaTypes.size() != 2) {
    return false;
  }

  // If no header field is present, then it is assumed
  // that the client accepts all media types.
  if (name.isNone()) {
    return true;
  }

  // Remove spaces and tabs for easier parsing.
  name = strings::remove(name.get(), " ");
  name = strings::remove(name.get(), "\t");
  name = strings::remove(name.get(), "\n");

  // First match 'type/subtype', then 'type/*', then '*/*'.
  vector<string> candidates;
  candidates.push_back(mediaType);
  candidates.push_back(mediaTypes[0] + "/*");
  candidates.push_back("*/*");

  foreach (const string& candidate, candidates) {
    foreach (const string& type, strings::tokenize(name.get(), ",")) {
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
  synchronized (data->lock) {
    if (data->readEnd == Reader::CLOSED) {
      return Failure("closed");
    } else if (!data->writes.empty()) {
      Future<string> future = data->writes.front();
      data->writes.pop();
      return future;
    } else if (data->writeEnd == Writer::CLOSED) {
      return ""; // End-of-file.
    } else if (data->writeEnd == Writer::FAILED) {
      CHECK_SOME(data->failure);
      return data->failure.get();
    } else {
      data->reads.push(Owned<Promise<string>>(new Promise<string>()));
      return data->reads.back()->future();
    }
  }
}


Future<string> Pipe::Reader::readAll()
{
  Pipe::Reader reader = *this;

  std::shared_ptr<string> buffer(new string());

  return loop(
      None(),
      [=]() mutable {
        return reader.read();
      },
      [=](const string& data) -> ControlFlow<string> {
        if (data.empty()) { // EOF.
          return Break(std::move(*buffer));
        }
        buffer->append(data);
        return Continue();
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
    read->set(std::move(s)); // NOLINT(misc-use-after-move)
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


namespace header {

Try<WWWAuthenticate> WWWAuthenticate::create(const string& input)
{
  // Set `maxTokens` as 2 since auth-param quoted string may
  // contain space (e.g., "Basic realm="Registry Realm").
  vector<string> tokens = strings::tokenize(input, " ", 2);
  if (tokens.size() != 2) {
    return Error("Unexpected WWW-Authenticate header format: '" + input + "'");
  }

  // Since the authentication parameters can contain quote values, we
  // do not use `strings::split` here since the delimiter may occur in
  // a quoted value which should not be split.
  hashmap<string, string> authParam;
  Option<string> key, value;
  bool inQuotes = false;

  foreach (char c, tokens[1]) {
    // Auth-param values can be a quoted-string or directive values.
    // Please see section "3.2.2.4 Directive values and quoted-string":
    // https://tools.ietf.org/html/rfc2617.
    //
    // If we see a quote we know we must already be parsing `value`
    // since `key` cannot be a quoted-string.
    if (c != '"' && inQuotes) {
      if (value.isNone()) {
        return Error("Unexpected auth-param format: '" + tokens[1] + "'");
      }

      value->append({c});
      continue;
    }

    // If we have not yet parsed `key` this character must belong to
    // it if it is not a space, and cannot be a `,` delimiter.
    if (key.isNone()) {
      if (c == ',') {
        return Error("Unexpected auth-param format: '" + tokens[1] + "'");
      }

      if (c == ' ') {
        continue;
      }

      key = string({c});
      continue;
    }

    // If the current character is `=` we must start parsing a new
    // `value`. Since we have already handled `=` in quotes above we
    // cannot already have started parsing `value`.
    if (c == '=') {
      if (value.isSome()) {
        return Error("Unexpected auth-param format: '" + tokens[1] + "'");
      }
      value = "";
      continue;
    }

    // If the current character is a quote, drop the
    // character and toogle the quote state.
    if (c == '"') {
      inQuotes = !inQuotes;
      continue;
    }

    // If the current character is a record delimiter and we are not
    // in quotes, we should have parsed both a key and a value. Store
    // them, drop the delimiter, and restart parsing.
    if (c == ',' && !inQuotes) {
      if (key.isNone() || value.isNone()) {
        return Error("Unexpected auth-param format: '" + tokens[1] + "'");
      }
      authParam[*key] = *value;
      key = None();
      value = None();

      continue;
    }

    // If we have not started parsing `value` we are still parsing `key`.
    if (value.isNone()) {
      key->append({c});
    } else {
      value->append({c});
    }
  }

  // Record the last parsed `(key, value)` pair.
  if (key.isSome()) {
    if (value.isNone() || inQuotes) {
      return Error("Unexpected auth-param format: '" + tokens[1] + "'");
    }
    authParam[*key] = *value;
  }

  // The realm directive (case-insensitive) is required for all
  // authentication schemes that issue a challenge.
  if (!authParam.contains("realm")) {
    return Error(
        "Unexpected auth-param '" +
        tokens[1] + "': 'realm' is not defined");
  }

  return WWWAuthenticate(tokens[0], authParam);
}


string WWWAuthenticate::authScheme()
{
  return authScheme_;
}


hashmap<string, string> WWWAuthenticate::authParam()
{
  return authParam_;
}

} // namespace header {


OK::OK(const JSON::Value& value, const Option<string>& jsonp)
  : Response(Status::OK)
{
  type = BODY;

  if (jsonp.isSome()) {
    headers["Content-Type"] = "text/javascript";

    string stringified = stringify(value);

    body.reserve(jsonp->size() + 1 + stringified.size() + 1);
    body += jsonp.get();
    body += "(";
    body += stringified;
    body += ")";
  } else {
    headers["Content-Type"] = "application/json";
    body = stringify(value);
  }

  headers["Content-Length"] = stringify(body.size());
}


OK::OK(JSON::Proxy&& value, const Option<string>& jsonp)
  : Response(Status::OK)
{
  type = BODY;

  if (jsonp.isSome()) {
    headers["Content-Type"] = "text/javascript";

    string stringified = std::move(value);

    body.reserve(jsonp->size() + 1 + stringified.size() + 1);
    body += jsonp.get();
    body += "(";
    body += stringified;
    body += ")";
  } else {
    headers["Content-Type"] = "application/json";
    body = std::move(value);
  }

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


string encode(const string& s, const string& additional_chars)
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
        if (c < 0x20 ||
            c > 0x7F ||
            additional_chars.find_first_of(c) != string::npos) {
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

  vector<Response> result;

  auto appendResult = [&result](const deque<http::Response*>& responses) {
    foreach (Response* response, responses) {
      result.push_back(*response);
      delete response;
    }
  };

  appendResult(decoder.decode(s.data(), s.length()));
  appendResult(decoder.decode("", 0));

  if (decoder.failed()) {
    return Error("Decoding failed");
  }

  if (result.empty()) {
    return Error("No response decoded");
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
  // TODO(bevers): The '//' sequence only follows the ':' if
  // the next part begins with an "authority". This is required
  // for the 'http' scheme, but not guaranteed in general.
  if (url.scheme.isSome()) {
    stream << url.scheme.get() << "://";
  }

  if (url.domain.isSome()) {
    // Optimize the most common case by not encoding for `http`
    // and `https` schemes, since their domain part is already
    // guaranteed to only contain unreserved characters.
    bool encodeDomain = url.scheme.isNone() ||
      (*url.scheme != "http" && *url.scheme != "https");

    const std::string& host = encodeDomain
      ? http::encode(url.domain.get())
      : url.domain.get();

    stream << host;
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
      query.push_back(http::encode(key) + "=" + http::encode(value));
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

  if (request.url.scheme == Some("http+unix")) {
    headers["Host"] = "localhost";
  } else if (request.url.domain.isSome()) {
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
      Pipe::Reader requestReader = request.reader.get();
      loop(None(),
           [=]() mutable {
             return requestReader.read();
           },
           [=](const string& chunk) mutable -> ControlFlow<Nothing> {
             if (chunk.empty()) {
               // EOF case.
               writer.write("0\r\n\r\n");
               writer.close();
               return Break();
             }

             std::ostringstream out;
             out << std::hex << chunk.size() << "\r\n";
             out << chunk;
             out << "\r\n";

             writer.write(out.str());

             return Continue();
           })
        .onDiscarded([=]() mutable {
          writer.fail("discarded");
        })
        .onFailed([=](const string& failure) mutable {
          writer.fail(failure);
        });
      break;
  }

  return reader;
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
  ConnectionProcess(const network::Socket& _socket)
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
    network::Socket socket_ = socket;

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
    Try<Nothing, SocketError> shutdown = socket.shutdown(
        network::Socket::Shutdown::READ_WRITE);

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

    disconnection.set(Nothing());

    return shutdown.isSome() ? Future<Nothing>(Nothing())
                             : Failure(shutdown.error().message);
  }

  Future<Nothing> disconnected()
  {
    return disconnection.future();
  }

protected:
  void initialize() override
  {
    // Start the read loop on the socket. We read independently
    // of the requests being sent in order to detect socket
    // closure at any time.
    read();
  }

  void finalize() override
  {
    disconnect("Connection object was destructed");
  }

private:
  static Future<Nothing> _send(network::Socket socket, Pipe::Reader reader)
  {
    return loop(
        None(),
        [=]() mutable {
          return reader.read();
        },
        [=](const string& data) mutable -> Future<ControlFlow<Nothing>> {
          if (data.empty()) {
            return Break(); // EOF.
          }
          return socket.send(data)
            .then([]() -> ControlFlow<Nothing> {
              return Continue();
            });
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

  network::Socket socket;
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
  Data(const network::Socket& s)
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


Connection::Connection(
    const network::Socket& s,
    const network::Address& _localAddress,
    const network::Address& _peerAddress)
  : localAddress(_localAddress), peerAddress(_peerAddress),
    data(std::make_shared<Connection::Data>(s)) {}


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


Future<Connection> connect(
    const network::Address& address,
    Scheme scheme,
    const Option<string>& peer_hostname)
{
  SocketImpl::Kind kind;

  switch (scheme) {
    case Scheme::HTTP:
    case Scheme::HTTP_UNIX:
      kind = SocketImpl::Kind::POLL;
      break;
#ifdef USE_SSL_SOCKET
    case Scheme::HTTPS:
      kind = SocketImpl::Kind::SSL;
      break;
#endif
  }

  Try<network::Socket> socket = network::Socket::create(
      address.family(),
      kind);

  if (socket.isError()) {
    return Failure("Failed to create socket: " + socket.error());
  }

  Future<Nothing> connected = [&]() {
    switch (scheme) {
      case Scheme::HTTP:
      case Scheme::HTTP_UNIX:
        return socket->connect(address);
#ifdef USE_SSL_SOCKET
      case Scheme::HTTPS:
        return socket->connect(
            address,
            network::openssl::create_tls_client_config(peer_hostname));
#endif
    }
    UNREACHABLE();
  }();

  return connected
    .then([socket, address]() -> Future<Connection> {
      Try<network::Address> localAddress = socket->address();
      if (localAddress.isError()) {
        return Failure("Failed to get socket's local address: " +
            localAddress.error());
      }

      return Connection(socket.get(), localAddress.get(), address);
    });
}


Future<Connection> connect(
    const network::Address& address,
    Scheme scheme)
{
  return connect(address, scheme, None());
}


Future<Connection> connect(const URL& url)
{
  // Default to 'http' if no scheme was specified.
  string scheme = url.scheme.getOrElse("http");
  bool inetAddressRequired = scheme == "http" || scheme == "https";

  // TODO(bmahler): Move address resolution into the URL class?
  Address address = inet4::Address::ANY_ANY();

  if (url.ip.isNone() && url.domain.isNone()) {
    return Failure("Expected URL.ip or URL.domain to be set");
  }

  if (url.ip.isSome()) {
    address.ip = url.ip.get();
  } else if (inetAddressRequired) {
    Try<net::IP> ip = net::getIP(url.domain.get(), AF_INET);

    if (ip.isError()) {
      return Failure("Failed to determine IP of domain '" +
                     url.domain.get() + "': " + ip.error());
    }

    address.ip = ip.get();
  }

  if (url.port.isNone() && inetAddressRequired) {
    return Failure("Expecting url.port to be set");
  }

  if (url.port.isSome()) {
    address.port = url.port.get();
  }

  if (scheme == "http") {
    return connect(address, Scheme::HTTP, url.domain);
  }

  if (scheme == "https") {
#ifdef USE_SSL_SOCKET
    return connect(address, Scheme::HTTPS, url.domain);
#else
    return Failure("'https' scheme requires SSL enabled");
#endif
  }

#ifndef __WINDOWS__
  if (scheme == "http+unix") {
    if (!url.domain.isSome()) {
      return Failure("'http+unix' scheme requires domain (filesystem path)");
    }

    Try<network::unix::Address> address =
      network::unix::Address::create(url.domain.get());

    if (address.isError()) {
      return Failure(strings::format(
          "Could not create address from %s: %s",
          url.domain.get(),
          address.error()).get());
    }

    return connect(*address, Scheme::HTTP, url.domain);
  }
#endif // __WINDOWS__

  return Failure("Unsupported URL scheme");
}


namespace internal {

Future<Nothing> send(network::Socket socket, Encoder* encoder)
{
  size_t* size = new size_t(0);
  return loop(
      None(),
      [=]() {
        switch (encoder->kind()) {
          case Encoder::DATA: {
            const char* data = static_cast<DataEncoder*>(encoder)->next(size);
            return socket.send(data, *size);
          }
          case Encoder::FILE: {
            off_t offset = 0;
            int_fd fd = static_cast<FileEncoder*>(encoder)->next(&offset, size);
            return socket.sendfile(fd, offset, *size);
          }
        }
        UNREACHABLE();
      },
      [=](size_t length) -> ControlFlow<Nothing> {
        // Update the encoder with the amount sent.
        encoder->backup(*size - length);

        // See if there is any more of the message to send.
        if (encoder->remaining() != 0) {
          return Continue();
        }

        return Break();
      })
    .onAny([=]() {
      delete size;
    });
}


Future<Nothing> send(
    network::Socket socket,
    const Response& response,
    Request* request)
{
  CHECK(response.type == Response::BODY ||
        response.type == Response::NONE);

  Encoder* encoder = new HttpResponseEncoder(response, *request);

  return send(socket, encoder)
    .onAny([=]() {
      delete encoder;
    });
}


Future<Nothing> sendfile(
    network::Socket socket,
    Response response,
    Request* request)
{
  CHECK(response.type == Response::PATH);

  // Make sure no body is sent (this is really an error and
  // should be reported and no response sent.
  response.body.clear();

  Try<int_fd> fd = os::open(response.path, O_CLOEXEC | O_NONBLOCK | O_RDONLY);

  if (fd.isError()) {
    const string body = "Failed to open '" + response.path + "': " + fd.error();
    // TODO(benh): VLOG(1)?
    // TODO(benh): Don't send error back as part of InternalServiceError?
    // TODO(benh): Copy headers from `response`?
    return send(socket, InternalServerError(body), request);
  }

  const Try<Bytes> size = os::stat::size(fd.get());
  if (size.isError()) {
    const string body =
      "Failed to fstat '" + response.path + "': " + size.error();
    // TODO(benh): VLOG(1)?
    // TODO(benh): Don't send error back as part of InternalServiceError?
    // TODO(benh): Copy headers from `response`?
    os::close(fd.get());
    return send(socket, InternalServerError(body), request);
  } else if (os::stat::isdir(fd.get())) {
    const string body = "'" + response.path + "' is a directory";
    // TODO(benh): VLOG(1)?
    // TODO(benh): Don't send error back as part of InternalServiceError?
    // TODO(benh): Copy headers from `response`?
    os::close(fd.get());
    return send(socket, InternalServerError(body), request);
  }

  // While the user is expected to properly set a 'Content-Type'
  // header, we'll fill in (or overwrite) 'Content-Length' header.
  response.headers["Content-Length"] = stringify(size->bytes());

  // TODO(benh): If this is a TCP socket consider turning on TCP_CORK
  // for both sends and then turning it off.
  Encoder* encoder = new HttpResponseEncoder(response, *request);

  return send(socket, encoder)
    .onAny([=](const Future<Nothing>& future) {
      delete encoder;

      // Close file descriptor if we aren't doing any more sending.
      if (future.isDiscarded() || future.isFailed()) {
        os::close(fd.get());
      }
    })
    .then([=]() mutable -> Future<Nothing> {
      // NOTE: the file descriptor gets closed by FileEncoder.
      Encoder* encoder = new FileEncoder(fd.get(), size->bytes());
      return send(socket, encoder)
        .onAny([=]() {
          delete encoder;
        });
    });
}


Future<Nothing> stream(
    const network::Socket& socket,
    http::Pipe::Reader reader)
{
  return loop(
      None(),
      [=]() mutable {
        return reader.read();
      },
      [=](const string& data) mutable {
        bool finished = false;

        ostringstream out;

        if (data.empty()) {
          // Finished reading.
          out << "0\r\n" << "\r\n";
          finished = true;
        } else {
          out << std::hex << data.size() << "\r\n";
          out << data;
          out << "\r\n";
        }

        Encoder* encoder = new DataEncoder(out.str());

        return send(socket, encoder)
          .onAny([=]() {
            delete encoder;
          })
          .then([=]() mutable -> ControlFlow<Nothing> {
            if (!finished) {
              return Continue();
            }

            return Break();
          });
      });
}


Future<Nothing> stream(
    const network::Socket& socket,
    Response response,
    Request* request)
{
  CHECK(response.type == Response::PIPE);

  // Make sure no body is sent (this is really an error and
  // should be reported and no response sent).
  response.body.clear();

  if (response.reader.isNone()) {
    // This is clearly a programmer error, we don't have a reader from
    // which to stream! We return an `InternalServerError` rather than
    // failing just as we do in `sendfile` when we get a malformed
    // response.
    const string body = "Missing data to stream";
    // TODO(benh): VLOG(1)?
    // TODO(benh): Don't send error back as part of InternalServiceError?
    // TODO(benh): Copy headers from `response`?
    return send(socket, InternalServerError(body), request);
  }

  // While the user is expected to properly set a 'Content-Type'
  // header, we'll fill in (or overwrite) 'Transfer-Encoding' header.
  response.headers["Transfer-Encoding"] = "chunked";

  Encoder* encoder = new HttpResponseEncoder(response, *request);

  return send(socket, encoder)
    .onAny([=]() {
      delete encoder;
    })
    .then([=]() {
      return stream(socket, response.reader.get());
    })
    // Regardless of whether `send` or `stream` completed successfully
    // or failed we close the reader so any writers will be notified.
    .onAny([=]() mutable {
      response.reader->close();
    });
}


struct Item
{
  Request* request;
  Future<Response> response;
};


Future<Nothing> send(
    network::Socket socket,
    Queue<Option<Item>> pipeline)
{
  return loop(
      [=]() mutable {
        return pipeline.get();
      },
      [=](const Option<Item>& item) -> Future<ControlFlow<Nothing>> {
        if (item.isNone()) {
          return Break();
        }

        Request* request = item->request;
        Future<Response> response = item->response;

        // We do an `await` here so that we won't wait forever in the
        // event that we've discarded the loop but `response` is
        // abandoned or taking too long.
        return await(response)
          .recover([](const Future<Future<Response>>& future)
              -> Future<Response> {
            if (future.isFailed()) {
              return InternalServerError(
                  "Failed to wait for response: " + future.failure());
            }

            // Either `response` was abandoned or the loop has been
            // discarded so we return `ServiceUnavailable` so we can
            // continue the loop or break out if we've been discarded.
            return ServiceUnavailable();
          })
          .then([](const Future<Response>& future) -> Response {
            if (future.isFailed()) {
              return InternalServerError(future.failure());
            } else if (future.isDiscarded()) {
              return ServiceUnavailable();
            }
            return future.get();
          })
          .then([=](const Response& response) {
            // TODO(benh): Should any generated InternalServerError
            // responses due to bugs in the Response passed to us cause
            // us to return a Failure here rather than keep processing
            // more requests/responses?
            return [&]() {
              switch (response.type) {
                case Response::PATH: return sendfile(socket, response, request);
                case Response::PIPE: return stream(socket, response, request);
                case Response::BODY:
                case Response::NONE: return send(socket, response, request);
              }
              UNREACHABLE();
            }()
            .then([=]() -> ControlFlow<Nothing> {
              // Persist the connection if the request expects it and
              // the response doesn't include 'Connection: close'.
              bool persist = request->keepAlive;
              if (response.headers.contains("Connection")) {
                if (response.headers.at("Connection") == "close") {
                  persist = false;
                }
              }
              if (persist) {
                return Continue();
              }
              return Break();
            });
          })
          .onAny([=]() {
            delete request;
          });
      });
}


Future<Nothing> receive(
    network::Socket socket,
    std::function<Future<Response>(const Request&)>&& f,
    Queue<Option<Item>> pipeline)
{
  // Get the peer address to augment any requests we receive.
  Try<network::Address> address = socket.peer();

  if (address.isError()) {
    return Failure("Failed to get peer address: " + address.error());
  }

  const size_t size = io::BUFFERED_READ_SIZE;
  char* data = new char[size];

  StreamingRequestDecoder* decoder = new StreamingRequestDecoder();

  return loop(
      [=]() {
        return socket.recv(data, size);
      },
      [=](size_t length) mutable -> Future<ControlFlow<Nothing>> {
        if (length == 0) {
          return Break();
        }

        // Decode as much of the data as possible into HTTP requests.
        const deque<Request*> requests = decoder->decode(data, length);

        // NOTE: it's possible the decoder has failed but some
        // requests might be available, i.e., `requests.empty()` is
        // not true, so we wait to return a `Failure` until when there
        // are no requests.

        if (decoder->failed() && requests.empty()) {
          return Failure("Decoder error while receiving");
        }

        foreach (Request* request, requests) {
          request->client = address.get();
          // TODO(benh): To support HTTP pipelining we invoke `f`
          // regardless of whether the previous response has been
          // completed. This can make handling of requests more
          // difficult so we could consider supporting disabling HTTP
          // pipelining via some sort of "options" initially passed
          // in.
          pipeline.put(Item{request, f(*request)});
        }

        return Continue(); // Keep looping!
      })
    .onAny([=]() {
      delete decoder;
      delete[] data;
    });
}


Future<Nothing> serve(
    network::Socket socket,
    std::function<Future<Response>(const Request&)>&& f)
{
  // HTTP serving is implemented by running two loops, a "receive"
  // loop and a "send" loop. The receive loop passes the pipeline of
  // request/responses via a `Queue` to the send loop that is
  // responsible for sending the response back to the client. A `None`
  // passed on the queue signifies that receiving has completed.
  //
  // TODO(benh): Replace this with something like `Stream` that can
  // give us completion semantics without having to encode them with
  // an `Option` like we do here.
  Queue<Option<Item>> pipeline;

  Future<Nothing> receiving =
    receive(socket, std::move(f), pipeline)
      .onAny([=]() mutable {
        // Either:
        //
        //   (1) An EOF was received.
        //   (2) A failure occurred while receiving.
        //   (3) Receiving was discarded (likely because serving was
        //       discarded).
        //
        // In all cases the best course of action is to signify that
        // no more items will be enqueued on the `pipeline` and in
        // the case of (2) or (3) shutdown the read end of the
        // socket so the client recognizes it can't send any more
        // requests.
        //
        // Note that we don't look at the return value of
        // `Socket::shutdown` because the socket might already be
        // shutdown!
        pipeline.put(None());
        socket.shutdown(network::Socket::Shutdown::READ);
      });

  Future<Nothing> sending =
    send(socket, pipeline)
      .onAny([=]() mutable {
        // Either:
        //
        //   (1) HTTP connection is not meant to be persistent or
        //       there are no more items expected in the pipeline.
        //   (2) A failure occurred while sending.
        //   (3) Sending was discarded (likely because serving was
        //       discarded).
        //
        // In all cases the best course of action is to shutdown the
        // socket which will also force receiving to complete.
        //
        // Note that we don't look at the return value of
        // `Socket::shutdown` because the socket might already be
        // shutdown!
        //
        // CAREFUL! We can't shutdown with Shutdown::READ_WRITE
        // because on OSX if the socket is already shutdown with READ
        // due to the call above then the call will fail rather than
        // just treat it like a shutdown WRITE.
        socket.shutdown(network::Socket::Shutdown::READ);
        socket.shutdown(network::Socket::Shutdown::WRITE);
      });

  std::shared_ptr<Promise<Nothing>> promise(new Promise<Nothing>());

  promise->future().onDiscard([=]() mutable {
    receiving.discard();
    sending.discard();
  });

  await(sending, receiving)
    .onAny([=]() mutable {
      // Delete remaining requests and discard remaining responses.
      if (pipeline.size() != 0) {
        loop(None(),
             [=]() mutable {
               return pipeline.get();
             },
             [=](Option<Item> item) -> ControlFlow<Nothing> {
               if (item.isNone()) {
                 return Break();
               }
               delete item->request;
               if (promise->future().hasDiscard()) {
                 item->response.discard();
               }
               return Continue();
             });
      }

      if (receiving.isReady() && sending.isReady()) {
        promise->set(Nothing());
      } else if (receiving.isFailed() && sending.isFailed()) {
        promise->fail("Failed to receive (" + receiving.failure() +
                      ") and send (" + sending.failure() + ")");
      } else if (receiving.isFailed()) {
        promise->fail("Failed to receive: " + receiving.failure());
      } else if (sending.isFailed()) {
        promise->fail("Failed to send: " + sending.failure());
      } else {
        CHECK(receiving.isDiscarded() || sending.isDiscarded());
        promise->discard();
      }
    });

  return promise->future();
}

} // namespace internal {


class ServerProcess : public Process<ServerProcess>
{
public:
  ServerProcess(
      network::Socket&& socket,
      std::function<Future<Response>(
          const network::Socket&,
          const Request&)>&& f)
    : socket(std::move(socket)),
      f(std::move(f)),
      state(State::INITIALIZED) {}

  // `Server` implementation.
  Future<Nothing> run()
  {
    return state.transition<State::INITIALIZED, State::RUNNING>([=]() {
        // Start the accept loop and store the future so we can later
        // discard it when we need to stop the server.
        accepting = loop(
          self(),
          [=]() {
            return socket.accept();
          },
          [=](const network::Socket& socket) -> ControlFlow<Nothing> {
            // If we've transitioned to STOPPING we should break. It
            // may seem like we should never get here because we
            // discard the accept loop before we transition to
            // STOPPING but it's possible that we've already
            // dispatched this lambda and it is only now getting
            // invoked. It's critical that we break because after we
            // transition to STOPPING we assume that `clients` will
            // not be modified by the accept loop.
            if (state.is<State::STOPPING>()) {
              return Break();
            }

            Client client = {
              /* .socket = */ socket,
              /* .serving = */ http::serve(
                  socket,
                  [=](const Request& request) {
                    return f(socket, request);
                  })
            };

            clients.put(socket, client);

            client.serving
              .onAny(defer(self(), [=](const Future<Nothing>&) {
                clients.erase(socket);
              }));

            return Continue();
          });

      // We return a _discardable_ `accepting` so the caller can stop
      // running a server by "discarding the run", for example:
      //
      //   Future<Nothing> run = server.run();
      //   run.discard();
      //
      // Even if we returned an _undiscardable_ `accepting` we still
      // need to do a `recover()` on it in the event that the accept
      // loop fails (or is abandoned) so we can stop the server (if it
      // isn't already being stopped). If `accepting` completes
      // successfully then we must be stopping so just wait until
      // we've stopped!
      return accepting
        .then(defer(self(), [=]() {
          return state.when<State::STOPPED>();
        }))
        .recover(defer(self(), [=](const Future<Nothing>& future) {
          // If the accept loop completes because it is abandoned,
          // discarded, or failed we want to stop the server if it
          // isn't already being stopped. In fact, someone stopping
          // the server might be the reason we're executing this
          // callback because `stop()` discards `accepting`.
          //
          // If we're not already stopping, i.e., the state is
          // RUNNING, then it's safe to assume that the accept loop
          // has (1) failed or (2) been discarded because someone did
          // a discard on the future returned from `run()`. In either
          // of these cases we initiate a `stop()` ourselves and after
          // that completes return a failure capturing the reason we
          // had to initiate the stop.
          if (state.is<State::RUNNING>()) {
            return stop(Server::DEFAULT_STOP_OPTIONS())
              .then([=]() -> Future<Nothing> {
                return Failure(stringify(future));
              });
          }

          // Otherwise we must already be stopping so just wait till
          // we're stopped.
          return state.when<State::STOPPED>();
        }));
    });
  }

  Future<Nothing> stop(const Server::StopOptions& options)
  {
    return state.transition<State::RUNNING, State::STOPPING>([=]() {
      // We make stopping be undiscardable to ensure that we properly
      // cleanup. This has the added benefit of simplifying having to
      // reason about how discards may propagate here when someone
      // discards the future returned from `run()` after they've
      // already called `stop()`.
      return undiscardable([=]() {
        accepting.discard();

        // In addition to discarding the accept loop we also attempt
        // to stop accepting new clients via shutting down the
        // socket. Shutting down the socket on Linux will keep further
        // connections from being established even though we haven't
        // closed the socket but on OS X connections will keep queuing
        // (in fact, calling `shutdown()` is an error, hence we don't
        // check the return value).
        socket.shutdown(network::Socket::Shutdown::READ_WRITE);

        // TODO(benh): ideally we also try and shut down the read end
        // of existing clients to signal that we won't handle any new
        // requests. Note that doing this is expected by the
        // specification, see section 8.1.4 in
        // https://www.w3.org/Protocols/rfc2616/rfc2616-sec8.html. Something
        // like:
        //
        // foreachvalue (Client& client, clients) {
        //   client.socket.shutdown(network::Socket::Shutdown::READ);
        // }
        //
        // Doing this is non-trivial, however, because in practice
        // what most servers do is continue to read any partial
        // requests until some point at which no more requests appear
        // to be coming in. This will require significant changes in
        // `http::serve()`, hence it's left as a TODO.

        // Wait for the current clients to finish (we know no more
        // clients will get added because we set `stopping` and the
        // accept loop will respect that (see `run()`).
        return await(lambda::map(
            [](Client&& client) {
              return client.serving;
            },
            clients.values()))
          // After the grace period expires discard all the clients
          // and then keep waiting.
          .after(options.grace_period,
                 defer(self(), [=](Future<vector<Future<Nothing>>> f) {
                   f.discard();
                   return await(lambda::map(
                       [](Client&& client) {
                         client.serving.discard();
                         return client.serving;
                       },
                       clients.values()));
                 }))
          .then(defer(self(), [=]() {
            clients.clear();

            // We `await()` the accept loop because we don't care how
            // it completes, we just want it to complete.
            return await(accepting)
              .then(defer(self(), [=]() -> Future<Nothing> {
                return state.transition<State::STOPPING, State::STOPPED>();
              }));
          }));
      }());
    },
    "Server must be started in order to be stopped");
  }

protected:
  void finalize() override
  {
    // If we started the accept loop then discard it and any clients
    // we are already serving.
    accepting.discard();

    // NOTE: we know that no more sockets will be accepted because
    // the accept loop is on `self()` and we're in `finalize()`.
    foreachvalue (Client& client, clients) {
      client.serving.discard();
    }

    clients.clear();
  }

private:
  network::Socket socket;
  std::function<Future<Response>(const network::Socket&, const Request&)> f;

  enum class State
  {
    INITIALIZED,
    RUNNING,
    STOPPING,
    STOPPED,
  };

  StateMachine<State> state;

  Future<Nothing> accepting;

  struct Client
  {
    network::Socket socket;
    Future<Nothing> serving;
  };

  hashmap<int_fd, Client> clients;
};


Try<Server> Server::create(
    network::Socket socket,
    std::function<Future<Response>(const network::Socket&, const Request&)>&& f,
    const CreateOptions& options)
{
  // NOTE: we start listening on the socket here so that a client can
  // attempt to connect to the server even before `Server::run` has
  // been called. If we postpone calling `Socket::listen` until we
  // invoke `Server::run` there is a race between when `Server::run`
  // returns and when `Socket::listen` actually gets called because
  // `Server::run` dispatches to `ServerProcess::run`. This is likely
  // not a problem in practice but is definitely problematic with
  // tests that try and start making connections immediately after
  // `Server::run` has returned but potentially before
  // `Socket::listen` has been invoked.
  Try<Nothing> listen = socket.listen(static_cast<int>(options.backlog));
  if (listen.isError()) {
    return Error("Failed to listen on socket: " + listen.error());
  }

  return Server(std::move(socket), std::move(f));
}


Try<Server> Server::create(
    const network::Address& address,
    std::function<Future<Response>(const network::Socket&, const Request&)>&& f,
    const Server::CreateOptions& options)
{
  SocketImpl::Kind kind = [&]() {
    switch (options.scheme) {
      case Scheme::HTTP:
      case Scheme::HTTP_UNIX:
        return SocketImpl::Kind::POLL;
#ifdef USE_SSL_SOCKET
      case Scheme::HTTPS:
        return SocketImpl::Kind::SSL;
#endif
    }
    UNREACHABLE();
  }();

  Try<network::Socket> socket = network::Socket::create(
      address.family(),
      kind);

  if (socket.isError()) {
    return Error("Failed to create socket: " + socket.error());
  }

  Try<network::Address> bind = socket->bind(address);
  if (bind.isError()) {
    return Error(
        "Failed to bind to address '" + stringify(address) + "': "
        + bind.error());
  }

  return create(socket.get(), std::move(f), options);
}


Server::Server(
    network::Socket&& socket,
    std::function<Future<Response>(const network::Socket&, const Request&)>&& f)
  : socket(socket),
    process(new ServerProcess(std::move(socket), std::move(f)))
{
  spawn(*process);
}


Server::~Server()
{
  // `process` may be a `nullptr` if we've moved `this`.
  if (process.get() != nullptr) {
    terminate(*process);
    wait(*process);
  }
}


Future<Nothing> Server::run()
{
  return dispatch(*process, &ServerProcess::run);
}


Future<Nothing> Server::stop(const Server::StopOptions& options)
{
  return dispatch(*process, &ServerProcess::stop, options);
}


Try<network::Address> Server::address() const
{
  return socket.address();
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
  const string scheme = enableSSL ? "https" : "http";
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
    const Option<Headers>& headers,
    const Option<string>& scheme)
{
  URL url(
      scheme.getOrElse("http"),
      net::IP(upid.address.ip),
      upid.address.port,
      upid.id);

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
    const Option<string>& contentType,
    const Option<string>& scheme)
{
  URL url(
      scheme.getOrElse("http"),
      net::IP(upid.address.ip),
      upid.address.port,
      upid.id);

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
    const Option<Headers>& headers,
    const Option<string>& scheme)
{
  URL url(
      scheme.getOrElse("http"),
      net::IP(upid.address.ip),
      upid.address.port,
      upid.id);

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
    const Option<Headers>& headers,
    const Option<string>& scheme)
{
  URL url(
      scheme.getOrElse("http"),
      net::IP(upid.address.ip),
      upid.address.port,
      upid.id);

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
    const Option<string>& contentType,
    const Option<string>& scheme)
{
  URL url(
      scheme.getOrElse("http"),
      net::IP(upid.address.ip),
      upid.address.port,
      upid.id);

  if (path.isSome()) {
    // TODO(benh): Get 'query' and/or 'fragment' out of 'path'.
    url.path = strings::join("/", url.path, path.get());
  }

  return streaming::post(url, headers, body, contentType);
}

} // namespace streaming {

} // namespace http {
} // namespace process {
