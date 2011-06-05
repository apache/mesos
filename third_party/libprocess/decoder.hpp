#ifndef __DECODER_HPP__
#define __DECODER_HPP__

#include <http_parser.h>

#include <deque>
#include <string>
#include <vector>

#include "foreach.hpp"
#include "process.hpp"
#include "tokenize.hpp"


namespace process {

class Proxy : public Process<Proxy>
{
public:
  Proxy(int _c);
  ~Proxy();

protected:
  virtual void operator () ();

private:
  int c;
};


class DataDecoder
{
public:
  DataDecoder(int _c, uint32_t _ip, uint16_t _port)
    : c(_c), ip(_ip), port(_port), proxy(NULL), failure(false)
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

  std::deque<Message*> decode(const char* data, size_t length)
  {
    size_t parsed = http_parser_execute(&parser, &settings, data, length);

    if (parsed != length) {
      failure = true;
    }

    if (!messages.empty()) {
      std::deque<Message*> result = messages;
      messages.clear();
      return result;
    }

    return std::deque<Message*>();
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
    decoder->headers.clear();
    decoder->path.clear();
    decoder->url.clear();
    decoder->fragment.clear();
    decoder->query.clear();
    decoder->body.clear();
    return 0;
  }

  static int on_headers_complete(http_parser* p)
  {
    return 0;
  }

  static int on_message_complete(http_parser* p)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;

    // Okay, the message is complete, do the necessary parsing (it
    // would be cooler to do this inline instead).
    std::string name;
    UPID to, from;

    const std::vector<std::string>& pairs = tokenize(decoder->path, "/");
    if (pairs.size() != 2) {
      decoder->failure = true;
      return -1;
    }

    to = UPID(pairs[0], decoder->ip, decoder->port);
    name = pairs[1];

    if (name == "") {
      decoder->failure = true;
      return -1;
    }

    // Check the method after we are sure everthing else parsed
    // correctly so that we don't create a proxy if there is some
    // decoder error.
    if (decoder->parser.method == HTTP_POST) {
      if (decoder->headers.count("User-Agent") > 0) {
        const std::string& value = decoder->headers["User-Agent"];
        std::string libprocess = "libprocess/";
        size_t index = value.find(libprocess);
        if (index != std::string::npos) {
          from = value.substr(index + libprocess.size(), value.size());
        }
      }
    } else if (decoder->parser.method == HTTP_GET) {
      if (decoder->proxy == NULL) {
        decoder->proxy = new Proxy(decoder->c);
        from = spawn(decoder->proxy);
      } else {
        // TODO(benh): For now, we just drop this message on the floor
        // (by returning success but not actually creating a
        // message. In the future, it would be nice/cool to support
        // pipelined requests.
        return 0;
      }
    } else {
      decoder->failure = true;
      return -1;
    }

    Message* message = new Message();
    message->name = name;
    message->from = from;
    message->to = to;
    message->body = decoder->body;

//     std::cout << "name: " << message->name << std::endl;
//     std::cout << "from: " << message->from << std::endl;
//     std::cout << "to: " << message->to << std::endl;

    decoder->messages.push_back(message);

    return 0;
  }

  static int on_header_field(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;

    if (decoder->header != HEADER_FIELD) {
      decoder->headers[decoder->field] = decoder->value;
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
    decoder->value.append(data, length);
    decoder->header = HEADER_VALUE;
    return 0;
  }

  static int on_path(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    decoder->path.append(data, length);
    return 0;
  }

  static int on_url(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    decoder->url.append(data, length);
    return 0;
  }

  static int on_query_string(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    decoder->query.append(data, length);
    return 0;
  }

  static int on_fragment(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    decoder->fragment.append(data, length);
    return 0;
  }

  static int on_body(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    decoder->body.append(data, length);
    return 0;
  }

  int c;
  uint32_t ip;
  uint16_t port;

  Proxy* proxy;

  bool failure;

  http_parser parser;
  http_parser_settings settings;

  enum {
    HEADER_FIELD,
    HEADER_VALUE
  } header;

  std::string field;
  std::string value;

  std::map<std::string, std::string> headers;

  std::string path;
  std::string url;
  std::string fragment;
  std::string query;
  std::string body;

  std::deque<Message*> messages;
};

}  // namespace process {

#endif // __DECODER_HPP__
