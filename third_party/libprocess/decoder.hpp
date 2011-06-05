#ifndef __DECODER_HPP__
#define __DECODER_HPP__

#include <http_parser.h>

#include <deque>
#include <string>
#include <vector>

#include "foreach.hpp"
#include "http.hpp"


namespace process {

class DataDecoder
{
public:
  DataDecoder()
    : failure(false), request(NULL)
  {
    settings.on_message_begin = &DataDecoder::on_message_begin;
    settings.on_header_field = &DataDecoder::on_header_field;
    settings.on_header_value = &DataDecoder::on_header_value;
    settings.on_path = &DataDecoder::on_path;
    settings.on_url = &DataDecoder::on_url;
    settings.on_fragment = &DataDecoder::on_fragment;
    settings.on_query_string = &DataDecoder::on_query_string;
    settings.on_body = &DataDecoder::on_body;
    settings.on_headers_complete = &DataDecoder::on_headers_complete;
    settings.on_message_complete = &DataDecoder::on_message_complete;

    http_parser_init(&parser, HTTP_REQUEST);

    parser.data = this;
  }

  std::deque<HttpRequest*> decode(const char* data, size_t length)
  {
    size_t parsed = http_parser_execute(&parser, &settings, data, length);

    if (parsed != length) {
      failure = true;
    }

    if (!requests.empty()) {
      std::deque<HttpRequest*> result = requests;
      requests.clear();
      return result;
    }

    return std::deque<HttpRequest*>();
  }

  bool failed() const
  {
    return failure;
  }

private:
  static int on_message_begin(http_parser* p)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;

    assert(!decoder->failure);

    decoder->header = HEADER_FIELD;
    decoder->field.clear();
    decoder->value.clear();

    assert(decoder->request == NULL);
    decoder->request = new HttpRequest();
    decoder->request->headers.clear();
    decoder->request->method.clear();
    decoder->request->path.clear();
    decoder->request->url.clear();
    decoder->request->fragment.clear();
    decoder->request->query.clear();
    decoder->request->body.clear();

    return 0;
  }

  static int on_headers_complete(http_parser* p)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    decoder->request->method = http_method_str((http_method) decoder->parser.method);
    decoder->request->keepAlive = http_should_keep_alive(&decoder->parser);
    return 0;
  }

  static int on_message_complete(http_parser* p)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
//     std::cout << "HttpRequest:" << std::endl;
//     std::cout << "  method: " << decoder->request->method << std::endl;
//     std::cout << "  path: " << decoder->request->path << std::endl;
    decoder->requests.push_back(decoder->request);
    decoder->request = NULL;
    return 0;
  }

  static int on_header_field(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    assert(decoder->request != NULL);

    if (decoder->header != HEADER_FIELD) {
      decoder->request->headers[decoder->field] = decoder->value;
      decoder->field.clear();
      decoder->value.clear();
    }

    decoder->field.append(data, length);
    decoder->header = HEADER_FIELD;

    return 0;
  }

  static int on_header_value(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    assert(decoder->request != NULL);
    decoder->value.append(data, length);
    decoder->header = HEADER_VALUE;
    return 0;
  }

  static int on_path(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    assert(decoder->request != NULL);
    decoder->request->path.append(data, length);
    return 0;
  }

  static int on_url(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    assert(decoder->request != NULL);
    decoder->request->url.append(data, length);
    return 0;
  }

  static int on_query_string(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    assert(decoder->request != NULL);
    decoder->request->query.append(data, length);
    return 0;
  }

  static int on_fragment(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    assert(decoder->request != NULL);
    decoder->request->fragment.append(data, length);
    return 0;
  }

  static int on_body(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    assert(decoder->request != NULL);
    decoder->request->body.append(data, length);
    return 0;
  }

  bool failure;

  http_parser parser;
  http_parser_settings settings;

  enum {
    HEADER_FIELD,
    HEADER_VALUE
  } header;

  std::string field;
  std::string value;

  HttpRequest* request;

  std::deque<HttpRequest*> requests;
};

}  // namespace process {

#endif // __DECODER_HPP__
