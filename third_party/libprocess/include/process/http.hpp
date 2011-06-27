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
  // TODO(benh): Add major/minor version.
  std::string status;
  std::map<std::string, std::string> headers;
  // TODO(benh): Make body a stream (channel) instead, and allow a
  // response to be returned without forcing the stream to be
  // finished.
  std::string body;
};


struct HttpOKResponse : HttpResponse
{
  HttpOKResponse()
  {
    status = "200 OK";
  }
};


struct HttpBadRequestResponse : HttpResponse
{
  HttpBadRequestResponse()
  {
    status = "400 Bad Request";
  }
};


struct HttpNotFoundResponse : HttpResponse
{
  HttpNotFoundResponse()
  {
    status = "404 Not Found";
  }
};


struct HttpInternalServerErrorResponse : HttpResponse
{
  HttpInternalServerErrorResponse()
  {
    status = "500 Internal Server Error";
  }
};


struct HttpServiceUnavailableResponse : HttpResponse
{
  HttpServiceUnavailableResponse()
  {
    status = "503 Service Unavailable";
  }
};


} // namespace process {

#endif // __PROCESS_HTTP_HPP
