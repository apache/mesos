#ifndef __PROCESS_HTTP_HPP__
#define __PROCESS_HTTP_HPP__

#include <limits.h>
#include <stdint.h>

#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

#include <process/pid.hpp>

#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

namespace process {

// Forward declaration to break cyclic dependency.
template <typename T>
class Future;

namespace http {

struct Request
{
  // TODO(benh): Add major/minor version.
  // TODO(bmahler): Header names are not case sensitive! Either make these
  // case-insensitive, or add a variable for each header in HTTP 1.0/1.1 (like
  // we've done here with keepAlive).
  // Tracked by: https://issues.apache.org/jira/browse/MESOS-328.
  hashmap<std::string, std::string> headers;
  std::string method;
  std::string url; // (path?query#fragment)
  std::string path;
  std::string fragment;
  hashmap<std::string, std::string> query;
  std::string body;
  bool keepAlive;

  // Returns whether the encoding is considered acceptable in the request.
  // TODO(bmahler): Consider this logic being in decoder.hpp, and having the
  // Request contain a member variable for each popular HTTP 1.0/1.1 header.
  bool accepts(const std::string& encoding) const
  {
    // See RFC 2616, section 14.3 for the details.
    Option<std::string> accepted = headers.get("Accept-Encoding");

    if (accepted.isNone()) {
      return false;
    }

    // Remove spaces and tabs for easier parsing.
    accepted = strings::remove(accepted.get(), " ");
    accepted = strings::remove(accepted.get(), "\t");
    accepted = strings::remove(accepted.get(), "\n");

    // From RFC 2616:
    // 1. If the content-coding is one of the content-codings listed in
    //    the Accept-Encoding field, then it is acceptable, unless it is
    //    accompanied by a qvalue of 0. (As defined in section 3.9, a
    //    qvalue of 0 means "not acceptable.")
    // 2. The special "*" symbol in an Accept-Encoding field matches any
    //    available content-coding not explicitly listed in the header
    //    field.

    // First we'll look for the encoding specified explicitly, then '*'.
    std::vector<std::string> candidates;
    candidates.push_back(encoding);      // Rule 1.
    candidates.push_back("*");           // Rule 2.

    foreach (std::string& candidate, candidates) {
      // Is the candidate one of the accepted encodings?
      foreach (const std::string& _encoding,
               strings::tokenize(accepted.get(), ",")) {
        if (strings::startsWith(_encoding, candidate)) {
          // Is there a 0 q value? Ex: 'gzip;q=0.0'.
          const std::map<std::string, std::vector<std::string> >& values =
            strings::pairs(_encoding, ";", "=");

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

    // NOTE: 3 and 4 are partially ignored since we can only provide gzip.
    // 3. If multiple content-codings are acceptable, then the acceptable
    //    content-coding with the highest non-zero qvalue is preferred.
    // 4. The "identity" content-coding is always acceptable, unless
    //    specifically refused because the Accept-Encoding field includes
    //    "identity;q=0", or because the field includes "*;q=0" and does
    //    not explicitly include the "identity" content-coding. If the
    //    Accept-Encoding field-value is empty, then only the "identity"
    //    encoding is acceptable.
    return false;
  }
};


struct Response
{
  Response()
    : type(NONE)
  {}

  explicit Response(const std::string& _body)
    : type(BODY),
      body(_body)
  {
    headers["Content-Length"] = stringify(body.size());
  }

  // TODO(benh): Add major/minor version.
  std::string status;
  hashmap<std::string, std::string> headers;

  // Either provide a "body", an absolute "path" to a file, or a
  // "pipe" for streaming a response. Distinguish between the cases
  // using 'type' below.
  //
  // BODY: Uses 'body' as the body of the response. These may be
  // encoded using gzip for efficiency, if 'Content-Encoding' is not
  // already specified.
  //
  // PATH: Attempts to perform a 'sendfile' operation on the file
  // found at 'path'.
  //
  // PIPE: Splices data from 'pipe' using 'Transfer-Encoding=chunked'.
  // Note that the read end of the pipe will be closed by libprocess
  // either after the write end has been closed or if the socket the
  // data is being spliced to has been closed (i.e., nobody is
  // listening any longer). This can cause writes to the pipe to
  // generate a SIGPIPE (which will terminate your program unless you
  // explicitly ignore them or handle them).
  //
  // In all cases (BODY, PATH, PIPE), you are expected to properly
  // specify the 'Content-Type' header, but the 'Content-Length' and
  // or 'Transfer-Encoding' headers will be filled in for you.
  enum {
    NONE,
    BODY,
    PATH,
    PIPE
  } type;

  std::string body;
  std::string path;
  int pipe; // See comment above regarding the semantics for closing.
};


struct OK : Response
{
  OK()
  {
    status = "200 OK";
  }

  explicit OK(const char* body) : Response(std::string(body))
  {
    status = "200 OK";
  }

  explicit OK(const std::string& body) : Response(body)
  {
    status = "200 OK";
  }

  OK(const JSON::Value& value, const Option<std::string>& jsonp = None())
  {
    type = BODY;

    status = "200 OK";

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
};


struct Accepted : Response
{
  Accepted()
  {
    status = "202 Accepted";
  }

  explicit Accepted(const std::string& body) : Response(body)
  {
    status = "202 Accepted";
  }
};


struct TemporaryRedirect : Response
{
  explicit TemporaryRedirect(const std::string& url)
  {
    status = "307 Temporary Redirect";
    headers["Location"] = url;
  }
};

struct Unauthorized : Response
{
  Unauthorized(const std::string& realm)
  {
    status = "401 Unauthorized";
    headers["WWW-authenticate"] = "Basic realm=\"" + realm + "\"";
  }

  Unauthorized(const std::string& realm, const std::string& body)
    : Response(body)
  {
    status = "401 Unauthorized";
    headers["WWW-authenticate"] = "Basic realm=\"" + realm + "\"";
  }
};

struct BadRequest : Response
{
  BadRequest()
  {
    status = "400 Bad Request";
  }

  explicit BadRequest(const std::string& body)
    : Response(body)
  {
    status = "400 Bad Request";
  }
};


struct NotFound : Response
{
  NotFound()
  {
    status = "404 Not Found";
  }

  explicit NotFound(const std::string& body) : Response(body)
  {
    status = "404 Not Found";
  }
};


struct InternalServerError : Response
{
  InternalServerError()
  {
    status = "500 Internal Server Error";
  }

  explicit InternalServerError(const std::string& body) : Response(body)
  {
    status = "500 Internal Server Error";
  }
};


struct ServiceUnavailable : Response
{
  ServiceUnavailable()
  {
    status = "503 Service Unavailable";
  }

  explicit ServiceUnavailable(const std::string& body) : Response(body)
  {
    status = "503 Service Unavailable";
  }
};


namespace path {

// Parses an HTTP path into a map given a pattern (TODO(benh): Make
// the patterns be regular expressions). This returns an error if
// 'pattern' doesn't match 'path'. For example:
//
//   parse("/books/{isbn}/chapters/{chapter}",
//         "/books/0304827484/chapters/3")
//
// Would return a map with the following:
//   books: "books"
//   isbn: "0304827484"
//   chapters: "chapters"
//   chapter: "3"
//
// Another example:
//
//   parse("/books/{isbn}/chapters/{chapter}",
//         "/books/0304827484")
//
// Would return a map with the following:
//   books: "books"
//   isbn: "0304827484"
//
// And another:
//
//   parse("/books/{isbn}/chapters/{chapter}",
//         "/books/0304827484/chapters")
//
// Would return a map with the following:
//   books: "books"
//   isbn: "0304827484"
//   chapters: "chapters"
inline Try<hashmap<std::string, std::string> > parse(
    const std::string& pattern,
    const std::string& path)
{
  // Split the pattern by '/' into keys.
  std::vector<std::string> keys = strings::tokenize(pattern, "/");

  // Split the path by '/' into segments.
  std::vector<std::string> segments = strings::tokenize(path, "/");

  hashmap<std::string, std::string> result;

  while (!segments.empty()) {
    if (keys.empty()) {
      return Error(
          "Not expecting suffix '" + strings::join("/", segments) + "'");
    }

    std::string key = keys.front();

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


namespace query {

// Parses an HTTP query string into a map. For example:
//
//   parse("foo=1;bar=2;baz;foo=3")
//
// Would return a map with the following:
//   bar: "2"
//   baz: ""
//   foo: "3"
//
// We use the last value for a key for simplicity, since the RFC does not
// specify how to handle duplicate keys:
// http://en.wikipedia.org/wiki/Query_string
// TODO(bmahler): If needed, investigate populating the query map inline
// for better performance.
inline hashmap<std::string, std::string> parse(const std::string& query)
{
  hashmap<std::string, std::string> result;

  const std::vector<std::string>& tokens = strings::tokenize(query, ";&");
  foreach (const std::string& token, tokens) {
    const std::vector<std::string>& pairs = strings::split(token, "=");
    if (pairs.size() == 2) {
      result[pairs[0]] = pairs[1];
    } else if (pairs.size() == 1) {
      result[pairs[0]] = "";
    }
  }

  return result;
}

} // namespace query {


// Returns a percent-encoded string according to RFC 3986.
// The input string must not already be percent encoded.
inline std::string encode(const std::string& s)
{
  std::ostringstream out;

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


// Decodes a percent-encoded string according to RFC 3986.
// The input string must not already be decoded.
// Returns error on the occurrence of a malformed % escape in s.
inline Try<std::string> decode(const std::string& s)
{
  std::ostringstream out;

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
    std::istringstream in(s.substr(i + 1, 2));
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


// Sends a blocking HTTP GET request to the process with the given upid.
// Returns the HTTP response from the process, read asynchronously.
//
// TODO(bmahler): Have the request sent asynchronously as well.
// TODO(bmahler): For efficiency, this should properly use the ResponseDecoder
// on the read stream, rather than parsing the full string response at the end.
Future<Response> get(
    const UPID& upid,
    const Option<std::string>& path = None(),
    const Option<std::string>& query = None(),
    const Option<hashmap<std::string, std::string> >& headers = None());


// Sends a blocking HTTP POST request to the process with the given upid.
// Returns the HTTP response from the process, read asyncronously.
Future<Response> post(
    const UPID& upid,
    const Option<std::string>& path = None(),
    const Option<hashmap<std::string, std::string> >& headers = None(),
    const Option<std::string>& body = None(),
    const Option<std::string>& contentType = None());


// Status code reason strings, from the HTTP1.1 RFC:
// http://www.w3.org/Protocols/rfc2616/rfc2616-sec6.html
extern hashmap<uint16_t, std::string> statuses;


inline void initialize()
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


} // namespace http {
} // namespace process {

#endif // __PROCESS_HTTP_HPP__
