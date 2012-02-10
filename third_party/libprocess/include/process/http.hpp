#ifndef __PROCESS_HTTP_HPP__
#define __PROCESS_HTTP_HPP__

#include <string>

namespace process {

struct HttpRequest
{
  // TODO(benh): Add major/minor version.
  std::map<std::string, std::string> headers;
  std::string method;
  std::string path;
  std::string url;
  std::string fragment;
  std::string query;
  std::string body;
  bool keepAlive;
};


struct HttpResponse
{
  HttpResponse() : type(BODY) {}

  // TODO(benh): Add major/minor version.
  std::string status;
  std::map<std::string, std::string> headers;

  // TODO(benh): Make body a stream (channel) instead, and allow a
  // response to be returned without forcing the stream to be
  // finished.

  // Either provide a 'body' or an absolute 'path' to a file. If a
  // path is specified then we will attempt to perform a 'sendfile'
  // operation on the file. In either case you are expected to
  // properly specify the 'Content-Type' header, but the
  // 'Content-Length' header will be filled in for you if you specify
  // a path. Distinguish between the two using 'type' below.
  enum {
    BODY,
    PATH
  } type;

  std::string body;
  std::string path;
};


struct HttpOKResponse : HttpResponse
{
  HttpOKResponse() : HttpResponse()
  {
    status = "200 OK";
  }
};


struct HttpBadRequestResponse : HttpResponse
{
  HttpBadRequestResponse() : HttpResponse()
  {
    status = "400 Bad Request";
  }
};


struct HttpNotFoundResponse : HttpResponse
{
  HttpNotFoundResponse() : HttpResponse()
  {
    status = "404 Not Found";
  }
};


struct HttpInternalServerErrorResponse : HttpResponse
{
  HttpInternalServerErrorResponse() : HttpResponse()
  {
    status = "500 Internal Server Error";
  }
};


struct HttpServiceUnavailableResponse : HttpResponse
{
  HttpServiceUnavailableResponse() : HttpResponse()
  {
    status = "503 Service Unavailable";
  }
};


} // namespace process {

#endif // __PROCESS_HTTP_HPP
