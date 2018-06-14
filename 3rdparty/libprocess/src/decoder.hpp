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

#ifndef __DECODER_HPP__
#define __DECODER_HPP__

// `http_parser.h` defines an enum `flags` which conflicts
// with, e.g., a namespace in stout. Rename it with a macro.
#define flags http_parser_flags
#include <http_parser.h>
#undef flags

#include <glog/logging.h>

#include <deque>
#include <limits>
#include <string>
#include <vector>

#include <process/http.hpp>

#include <stout/foreach.hpp>
#include <stout/gzip.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>


#if !(HTTP_PARSER_VERSION_MAJOR >= 2)
#error HTTP Parser version >= 2 required.
#endif


namespace process {

namespace http_parsing {

// We expect callbacks to return 0 on success and 1 on failure. These constants
// are introduced solely to make decoders' code easier to read and are not meant
// to be used outside.
constexpr int SUCCESS = 0;
constexpr int FAILURE = 1;

} // namespace http_parsing {

// TODO(benh): Make DataDecoder abstract and make RequestDecoder a
// concrete subclass.
class DataDecoder
{
public:
  DataDecoder()
    : failure(false), request(nullptr)
  {
    http_parser_settings_init(&settings);

    settings.on_message_begin = &DataDecoder::on_message_begin;
    settings.on_url = &DataDecoder::on_url;
    settings.on_header_field = &DataDecoder::on_header_field;
    settings.on_header_value = &DataDecoder::on_header_value;
    settings.on_headers_complete = &DataDecoder::on_headers_complete;
    settings.on_body = &DataDecoder::on_body;
    settings.on_message_complete = &DataDecoder::on_message_complete;
    settings.on_chunk_complete = &DataDecoder::on_chunk_complete;
    settings.on_chunk_header = &DataDecoder::on_chunk_header;

    http_parser_init(&parser, HTTP_REQUEST);

    parser.data = this;
  }

  ~DataDecoder()
  {
    delete request;

    foreach (http::Request* request, requests) {
      delete request;
    }
  }

  std::deque<http::Request*> decode(const char* data, size_t length)
  {
    size_t parsed = http_parser_execute(&parser, &settings, data, length);

    if (parsed != length) {
      // TODO(bmahler): joyent/http-parser exposes error reasons.
      failure = true;
    }

    if (!requests.empty()) {
      std::deque<http::Request*> result = requests;
      requests.clear();
      return result;
    }

    return std::deque<http::Request*>();
  }

  bool failed() const
  {
    return failure;
  }

private:
  static int on_message_begin(http_parser* p)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;

    CHECK(!decoder->failure);

    decoder->header = HEADER_FIELD;
    decoder->field.clear();
    decoder->value.clear();
    decoder->query.clear();
    decoder->url.clear();

    CHECK(decoder->request == nullptr);

    decoder->request = new http::Request();

    return http_parsing::SUCCESS;
  }

  static int on_chunk_complete(http_parser* p)
  {
    return http_parsing::SUCCESS;
  }

  static int on_chunk_header(http_parser* p)
  {
    return http_parsing::SUCCESS;
  }

  static int on_url(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    CHECK_NOTNULL(decoder->request);

    // The current http_parser library (version 2.6.2 and below)
    // does not support incremental parsing of URLs. To compensate
    // we incrementally collect the data and parse it in
    // `on_message_complete`.
    decoder->url.append(data, length);

    return http_parsing::SUCCESS;
  }

  static int on_header_field(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    CHECK_NOTNULL(decoder->request);

    if (decoder->header != HEADER_FIELD) {
      decoder->request->headers[decoder->field] = decoder->value;
      decoder->field.clear();
      decoder->value.clear();
    }

    decoder->field.append(data, length);
    decoder->header = HEADER_FIELD;

    return http_parsing::SUCCESS;
  }

  static int on_header_value(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    CHECK_NOTNULL(decoder->request);
    decoder->value.append(data, length);
    decoder->header = HEADER_VALUE;
    return http_parsing::SUCCESS;
  }

  static int on_headers_complete(http_parser* p)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;

    CHECK_NOTNULL(decoder->request);

    // Add final header.
    decoder->request->headers[decoder->field] = decoder->value;
    decoder->field.clear();
    decoder->value.clear();

    decoder->request->method =
      http_method_str((http_method) decoder->parser.method);

    decoder->request->keepAlive = http_should_keep_alive(&decoder->parser) != 0;

    return http_parsing::SUCCESS;
  }

  static int on_body(http_parser* p, const char* data, size_t length)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;
    CHECK_NOTNULL(decoder->request);
    decoder->request->body.append(data, length);
    return http_parsing::SUCCESS;
  }

  static int on_message_complete(http_parser* p)
  {
    DataDecoder* decoder = (DataDecoder*) p->data;

    CHECK_NOTNULL(decoder->request);

    // Parse the URL. This data was incrementally built up during calls
    // to `on_url`.
    http_parser_url url;
    http_parser_url_init(&url);
    int parse_url =
      http_parser_parse_url(decoder->url.data(), decoder->url.size(), 0, &url);

    if (parse_url != 0) {
      decoder->failure = true;
      return parse_url;
    }

    if (url.field_set & (1 << UF_PATH)) {
      decoder->request->url.path = std::string(
          decoder->url.data() + url.field_data[UF_PATH].off,
          url.field_data[UF_PATH].len);
    }

    if (url.field_set & (1 << UF_FRAGMENT)) {
      decoder->request->url.fragment = std::string(
          decoder->url.data() + url.field_data[UF_FRAGMENT].off,
          url.field_data[UF_FRAGMENT].len);
    }

    if (url.field_set & (1 << UF_QUERY)) {
      decoder->query = std::string(
          decoder->url.data() + url.field_data[UF_QUERY].off,
          url.field_data[UF_QUERY].len);
    }

    // Parse the query key/values.
    Try<hashmap<std::string, std::string>> decoded =
      http::query::decode(decoder->query);

    if (decoded.isError()) {
      decoder->failure = true;
      return http_parsing::FAILURE;
    }

    decoder->request->url.query = decoded.get();

    Option<std::string> encoding =
      decoder->request->headers.get("Content-Encoding");

    if (encoding.isSome() && encoding.get() == "gzip") {
      Try<std::string> decompressed = gzip::decompress(decoder->request->body);
      if (decompressed.isError()) {
        decoder->failure = true;
        return http_parsing::FAILURE;
      }
      decoder->request->body = decompressed.get();

      CHECK_LE(static_cast<long>(decoder->request->body.length()),
        std::numeric_limits<char>::max());

      decoder->request->headers["Content-Length"] =
        static_cast<char>(decoder->request->body.length());
    }

    decoder->requests.push_back(decoder->request);
    decoder->request = nullptr;
    return http_parsing::SUCCESS;
  }

  bool failure;

  http_parser parser;
  http_parser_settings settings;

  enum
  {
    HEADER_FIELD,
    HEADER_VALUE
  } header;

  std::string field;
  std::string value;
  std::string query;
  std::string url;

  http::Request* request;

  std::deque<http::Request*> requests;
};


class ResponseDecoder
{
public:
  ResponseDecoder()
    : failure(false), header(HEADER_FIELD), response(nullptr)
  {
    http_parser_settings_init(&settings);

    settings.on_message_begin = &ResponseDecoder::on_message_begin;
    settings.on_url = &ResponseDecoder::on_url;
    settings.on_header_field = &ResponseDecoder::on_header_field;
    settings.on_header_value = &ResponseDecoder::on_header_value;
    settings.on_headers_complete = &ResponseDecoder::on_headers_complete;
    settings.on_body = &ResponseDecoder::on_body;
    settings.on_message_complete = &ResponseDecoder::on_message_complete;
    settings.on_status = &ResponseDecoder::on_status;
    settings.on_chunk_complete = &ResponseDecoder::on_chunk_complete;
    settings.on_chunk_header = &ResponseDecoder::on_chunk_header;

    http_parser_init(&parser, HTTP_RESPONSE);

    parser.data = this;
  }

  ~ResponseDecoder()
  {
    delete response;

    foreach (http::Response* response, responses) {
      delete response;
    }
  }

  std::deque<http::Response*> decode(const char* data, size_t length)
  {
    size_t parsed = http_parser_execute(&parser, &settings, data, length);

    if (parsed != length) {
      // TODO(bmahler): joyent/http-parser exposes error reasons.
      failure = true;
    }

    if (!responses.empty()) {
      std::deque<http::Response*> result = responses;
      responses.clear();
      return result;
    }

    return std::deque<http::Response*>();
  }

  bool failed() const
  {
    return failure;
  }

private:
  static int on_message_begin(http_parser* p)
  {
    ResponseDecoder* decoder = (ResponseDecoder*) p->data;

    CHECK(!decoder->failure);

    decoder->header = HEADER_FIELD;
    decoder->field.clear();
    decoder->value.clear();

    CHECK(decoder->response == nullptr);

    decoder->response = new http::Response();
    decoder->response->status.clear();
    decoder->response->headers.clear();
    decoder->response->type = http::Response::BODY;
    decoder->response->body.clear();
    decoder->response->path.clear();

    return http_parsing::SUCCESS;
  }

  static int on_chunk_complete(http_parser* p)
  {
    return http_parsing::SUCCESS;
  }

  static int on_chunk_header(http_parser* p)
  {
    return http_parsing::SUCCESS;
  }

  static int on_url(http_parser* p, const char* data, size_t length)
  {
    return http_parsing::SUCCESS;
  }

  static int on_header_field(http_parser* p, const char* data, size_t length)
  {
    ResponseDecoder* decoder = (ResponseDecoder*) p->data;
    CHECK_NOTNULL(decoder->response);

    if (decoder->header != HEADER_FIELD) {
      decoder->response->headers[decoder->field] = decoder->value;
      decoder->field.clear();
      decoder->value.clear();
    }

    decoder->field.append(data, length);
    decoder->header = HEADER_FIELD;

    return http_parsing::SUCCESS;
  }

  static int on_header_value(http_parser* p, const char* data, size_t length)
  {
    ResponseDecoder* decoder = (ResponseDecoder*) p->data;
    CHECK_NOTNULL(decoder->response);
    decoder->value.append(data, length);
    decoder->header = HEADER_VALUE;
    return http_parsing::SUCCESS;
  }

  static int on_headers_complete(http_parser* p)
  {
    ResponseDecoder* decoder = (ResponseDecoder*) p->data;

    CHECK_NOTNULL(decoder->response);

    // Add final header.
    decoder->response->headers[decoder->field] = decoder->value;
    decoder->field.clear();
    decoder->value.clear();

    return http_parsing::SUCCESS;
  }

  static int on_body(http_parser* p, const char* data, size_t length)
  {
    ResponseDecoder* decoder = (ResponseDecoder*) p->data;
    CHECK_NOTNULL(decoder->response);
    decoder->response->body.append(data, length);
    return http_parsing::SUCCESS;
  }

  static int on_message_complete(http_parser* p)
  {
    ResponseDecoder* decoder = (ResponseDecoder*) p->data;

    CHECK_NOTNULL(decoder->response);

    if (http::isValidStatus(decoder->parser.status_code)) {
      decoder->response->code = decoder->parser.status_code;

      decoder->response->status =
        http::Status::string(decoder->parser.status_code);
    } else {
      decoder->failure = true;
      return http_parsing::FAILURE;
    }

    // We can only provide the gzip encoding.
    Option<std::string> encoding =
      decoder->response->headers.get("Content-Encoding");
    if (encoding.isSome() && encoding.get() == "gzip") {
      Try<std::string> decompressed = gzip::decompress(decoder->response->body);
      if (decompressed.isError()) {
        decoder->failure = true;
        return http_parsing::FAILURE;
      }
      decoder->response->body = decompressed.get();

      CHECK_LE(static_cast<long>(decoder->response->body.length()),
        std::numeric_limits<char>::max());

      decoder->response->headers["Content-Length"] =
        static_cast<char>(decoder->response->body.length());
    }

    decoder->responses.push_back(decoder->response);
    decoder->response = nullptr;
    return http_parsing::SUCCESS;
  }

  static int on_status(http_parser* p, const char* data, size_t length)
  {
    return http_parsing::SUCCESS;
  }

  bool failure;

  http_parser parser;
  http_parser_settings settings;

  enum
  {
    HEADER_FIELD,
    HEADER_VALUE
  } header;

  std::string field;
  std::string value;

  http::Response* response;

  std::deque<http::Response*> responses;
};


// Provides a response decoder that returns 'PIPE' responses once
// the response headers are received, but before the body data
// is received. Callers are expected to read the body from the
// Pipe::Reader in the response.
//
// TODO(bmahler): Consolidate streaming and non-streaming response
// decoders.
class StreamingResponseDecoder
{
public:
  StreamingResponseDecoder()
    : failure(false), header(HEADER_FIELD), response(nullptr)
  {
    http_parser_settings_init(&settings);

    settings.on_message_begin =
      &StreamingResponseDecoder::on_message_begin;
    settings.on_url =
      &StreamingResponseDecoder::on_url;
    settings.on_header_field =
      &StreamingResponseDecoder::on_header_field;
    settings.on_header_value =
      &StreamingResponseDecoder::on_header_value;
    settings.on_headers_complete =
      &StreamingResponseDecoder::on_headers_complete;
    settings.on_body =
      &StreamingResponseDecoder::on_body;
    settings.on_message_complete =
      &StreamingResponseDecoder::on_message_complete;
    settings.on_status =
      &StreamingResponseDecoder::on_status;
    settings.on_chunk_complete =
      &StreamingResponseDecoder::on_chunk_complete;
    settings.on_chunk_header =
      &StreamingResponseDecoder::on_chunk_header;

    http_parser_init(&parser, HTTP_RESPONSE);

    parser.data = this;
  }

  ~StreamingResponseDecoder()
  {
    delete response;

    if (writer.isSome()) {
      writer->fail("Decoder is being deleted");
    }

    foreach (http::Response* response, responses) {
      delete response;
    }
  }

  std::deque<http::Response*> decode(const char* data, size_t length)
  {
    size_t parsed = http_parser_execute(&parser, &settings, data, length);

    if (parsed != length) {
      // TODO(bmahler): joyent/http-parser exposes error reasons.
      failure = true;

      // If we're still writing the body, fail the writer!
      if (writer.isSome()) {
        http::Pipe::Writer writer_ = writer.get(); // Remove const.
        writer_.fail("failed to decode body");
        writer = None();
      }
    }

    if (!responses.empty()) {
      std::deque<http::Response*> result = responses;
      responses.clear();
      return result;
    }

    return std::deque<http::Response*>();
  }

  bool failed() const
  {
    return failure;
  }

  // Returns whether the decoder is currently writing a response
  // body. Helpful for knowing if the latest response is complete.
  bool writingBody() const
  {
    return writer.isSome();
  }

private:
  static int on_message_begin(http_parser* p)
  {
    StreamingResponseDecoder* decoder = (StreamingResponseDecoder*) p->data;

    CHECK(!decoder->failure);

    decoder->header = HEADER_FIELD;
    decoder->field.clear();
    decoder->value.clear();

    CHECK(decoder->response == nullptr);
    CHECK_NONE(decoder->writer);

    decoder->response = new http::Response();
    decoder->response->type = http::Response::PIPE;
    decoder->writer = None();

    return http_parsing::SUCCESS;
  }

  static int on_chunk_complete(http_parser* p)
  {
    return http_parsing::SUCCESS;
  }

  static int on_chunk_header(http_parser* p)
  {
    return http_parsing::SUCCESS;
  }

  static int on_status(http_parser* p, const char* data, size_t length)
  {
    return http_parsing::SUCCESS;
  }

  static int on_url(http_parser* p, const char* data, size_t length)
  {
    return http_parsing::SUCCESS;
  }

  static int on_header_field(http_parser* p, const char* data, size_t length)
  {
    StreamingResponseDecoder* decoder = (StreamingResponseDecoder*) p->data;

    // TODO(alexr): We currently do not support trailers, i.e., headers after
    // `on_headers_complete` has been called, and instead treat them as errors.
    if (decoder->response == nullptr) {
      return http_parsing::FAILURE;
    }

    if (decoder->header != HEADER_FIELD) {
      decoder->response->headers[decoder->field] = decoder->value;
      decoder->field.clear();
      decoder->value.clear();
    }

    decoder->field.append(data, length);
    decoder->header = HEADER_FIELD;

    return http_parsing::SUCCESS;
  }

  static int on_header_value(http_parser* p, const char* data, size_t length)
  {
    StreamingResponseDecoder* decoder = (StreamingResponseDecoder*) p->data;

    // TODO(alexr): We currently do not support trailers, i.e., headers after
    // `on_headers_complete` has been called, and instead treat them as errors.
    if (decoder->response == nullptr) {
      return http_parsing::FAILURE;
    }

    decoder->value.append(data, length);
    decoder->header = HEADER_VALUE;
    return http_parsing::SUCCESS;
  }

  static int on_headers_complete(http_parser* p)
  {
    StreamingResponseDecoder* decoder = (StreamingResponseDecoder*) p->data;

    // This asserts not only that `on_message_begin` has been previously called,
    // but also that `on_headers_complete` is not called more than once.
    CHECK_NOTNULL(decoder->response);

    // Add final header.
    decoder->response->headers[decoder->field] = decoder->value;
    decoder->field.clear();
    decoder->value.clear();

    if (http::isValidStatus(decoder->parser.status_code)) {
      decoder->response->code = decoder->parser.status_code;

      decoder->response->status =
        http::Status::string(decoder->parser.status_code);
    } else {
      decoder->failure = true;
      return http_parsing::FAILURE;
    }

    // We cannot provide streaming gzip decompression!
    Option<std::string> encoding =
      decoder->response->headers.get("Content-Encoding");
    if (encoding.isSome() && encoding.get() == "gzip") {
      decoder->failure = true;
      return http_parsing::FAILURE;
    }

    CHECK_NONE(decoder->writer);

    http::Pipe pipe;
    decoder->writer = pipe.writer();
    decoder->response->reader = pipe.reader();

    // Send the response to the caller, but keep a Pipe::Writer for
    // streaming the body content into the response.
    decoder->responses.push_back(decoder->response);

    // TODO(alexr): We currently do not support trailers, i.e., extra headers
    // after `on_headers_complete` has been called. When we add trailer support,
    // we need a thread-safe way to surface them up in the response or some
    // auxiliary data structure.
    decoder->response = nullptr;

    return http_parsing::SUCCESS;
  }

  static int on_body(http_parser* p, const char* data, size_t length)
  {
    StreamingResponseDecoder* decoder = (StreamingResponseDecoder*) p->data;

    CHECK_SOME(decoder->writer);

    http::Pipe::Writer writer = decoder->writer.get(); // Remove const.
    writer.write(std::string(data, length));

    return http_parsing::SUCCESS;
  }

  static int on_message_complete(http_parser* p)
  {
    StreamingResponseDecoder* decoder = (StreamingResponseDecoder*) p->data;

    // This can happen if the callback `on_headers_complete()` had failed
    // earlier (e.g., due to invalid status code).
    if (decoder->writer.isNone()) {
      CHECK(decoder->failure);
      return http_parsing::FAILURE;
    }

    http::Pipe::Writer writer = decoder->writer.get(); // Remove const.
    writer.close();

    decoder->writer = None();

    return http_parsing::SUCCESS;
  }

  bool failure;

  http_parser parser;
  http_parser_settings settings;

  enum
  {
    HEADER_FIELD,
    HEADER_VALUE
  } header;

  std::string field;
  std::string value;

  http::Response* response;
  Option<http::Pipe::Writer> writer;

  std::deque<http::Response*> responses;
};


// Provides a request decoder that returns 'PIPE' requests once
// the request headers are received, but before the body data
// is received. Callers are expected to read the body from the
// Pipe::Reader in the request.
class StreamingRequestDecoder
{
public:
  explicit StreamingRequestDecoder()
    : failure(false), header(HEADER_FIELD), request(nullptr)
  {
    http_parser_settings_init(&settings);

    settings.on_message_begin =
      &StreamingRequestDecoder::on_message_begin;
    settings.on_url =
      &StreamingRequestDecoder::on_url;
    settings.on_header_field =
      &StreamingRequestDecoder::on_header_field;
    settings.on_header_value =
      &StreamingRequestDecoder::on_header_value;
    settings.on_headers_complete =
      &StreamingRequestDecoder::on_headers_complete;
    settings.on_body =
      &StreamingRequestDecoder::on_body;
    settings.on_message_complete =
      &StreamingRequestDecoder::on_message_complete;
    settings.on_chunk_complete =
      &StreamingRequestDecoder::on_chunk_complete;
    settings.on_chunk_header =
      &StreamingRequestDecoder::on_chunk_header;

    http_parser_init(&parser, HTTP_REQUEST);

    parser.data = this;
  }

  ~StreamingRequestDecoder()
  {
    delete request;

    if (writer.isSome()) {
      writer->fail("Decoder is being deleted");
    }

    foreach (http::Request* request, requests) {
      delete request;
    }
  }

  std::deque<http::Request*> decode(const char* data, size_t length)
  {
    size_t parsed = http_parser_execute(&parser, &settings, data, length);
    if (parsed != length) {
      // TODO(bmahler): joyent/http-parser exposes error reasons.
      failure = true;

      // If we're still writing the body, fail the writer!
      if (writer.isSome()) {
        http::Pipe::Writer writer_ = writer.get(); // Remove const.
        writer_.fail("failed to decode body");
        writer = None();
      }
    }

    if (!requests.empty()) {
      std::deque<http::Request*> result = requests;
      requests.clear();
      return result;
    }

    return std::deque<http::Request*>();
  }

  bool failed() const
  {
    return failure;
  }

private:
  static int on_message_begin(http_parser* p)
  {
    StreamingRequestDecoder* decoder = (StreamingRequestDecoder*) p->data;

    CHECK(!decoder->failure);

    decoder->header = HEADER_FIELD;
    decoder->field.clear();
    decoder->value.clear();
    decoder->query.clear();
    decoder->url.clear();

    CHECK(decoder->request == nullptr);
    CHECK_NONE(decoder->writer);

    decoder->request = new http::Request();
    decoder->request->type = http::Request::PIPE;
    decoder->writer = None();
    decoder->decompressor.reset();

    return http_parsing::SUCCESS;
  }

  static int on_chunk_complete(http_parser* p)
  {
    return http_parsing::SUCCESS;
  }

  static int on_chunk_header(http_parser* p)
  {
    return http_parsing::SUCCESS;
  }

  static int on_url(http_parser* p, const char* data, size_t length)
  {
    StreamingRequestDecoder* decoder = (StreamingRequestDecoder*) p->data;

    // URL should not be parsed after `on_headers_complete` has been called.
    if (decoder->request == nullptr) {
      return http_parsing::FAILURE;
    }

    // The current http_parser library (version 2.6.2 and below)
    // does not support incremental parsing of URLs. To compensate
    // we incrementally collect the data and parse it in
    // `on_header_complete`.
    decoder->url.append(data, length);

    return http_parsing::SUCCESS;
  }

  static int on_header_field(http_parser* p, const char* data, size_t length)
  {
    StreamingRequestDecoder* decoder = (StreamingRequestDecoder*) p->data;

    // TODO(alexr): We currently do not support trailers, i.e., headers after
    // `on_headers_complete` has been called, and instead treat them as errors.
    if (decoder->request == nullptr) {
      return http_parsing::FAILURE;
    }

    if (decoder->header != HEADER_FIELD) {
      decoder->request->headers[decoder->field] = decoder->value;
      decoder->field.clear();
      decoder->value.clear();
    }

    decoder->field.append(data, length);
    decoder->header = HEADER_FIELD;

    return http_parsing::SUCCESS;
  }

  static int on_header_value(http_parser* p, const char* data, size_t length)
  {
    StreamingRequestDecoder* decoder = (StreamingRequestDecoder*) p->data;

    // TODO(alexr): We currently do not support trailers, i.e., headers after
    // `on_headers_complete` has been called, and instead treat them as errors.
    if (decoder->request == nullptr) {
      return http_parsing::FAILURE;
    }

    decoder->value.append(data, length);
    decoder->header = HEADER_VALUE;
    return http_parsing::SUCCESS;
  }

  static int on_headers_complete(http_parser* p)
  {
    StreamingRequestDecoder* decoder = (StreamingRequestDecoder*) p->data;

    // This asserts not only that `on_message_begin` has been previously called,
    // but also that `on_headers_complete` is not called more than once.
    CHECK_NOTNULL(decoder->request);

    // Add final header.
    decoder->request->headers[decoder->field] = decoder->value;
    decoder->field.clear();
    decoder->value.clear();

    decoder->request->method =
      http_method_str((http_method) decoder->parser.method);

    decoder->request->keepAlive = http_should_keep_alive(&decoder->parser) != 0;

    // Parse the URL. This data was incrementally built up during calls
    // to `on_url`.
    http_parser_url url;
    http_parser_url_init(&url);
    int parse_url =
      http_parser_parse_url(decoder->url.data(), decoder->url.size(), 0, &url);

    if (parse_url != 0) {
      decoder->failure = true;
      return parse_url;
    }

    if (url.field_set & (1 << UF_PATH)) {
      decoder->request->url.path = std::string(
          decoder->url.data() + url.field_data[UF_PATH].off,
          url.field_data[UF_PATH].len);
    }

    if (url.field_set & (1 << UF_FRAGMENT)) {
      decoder->request->url.fragment = std::string(
          decoder->url.data() + url.field_data[UF_FRAGMENT].off,
          url.field_data[UF_FRAGMENT].len);
    }

    if (url.field_set & (1 << UF_QUERY)) {
      decoder->query = std::string(
          decoder->url.data() + url.field_data[UF_QUERY].off,
          url.field_data[UF_QUERY].len);
    }

    // Parse the query key/values.
    Try<hashmap<std::string, std::string>> decoded =
      http::query::decode(decoder->query);

    if (decoded.isError()) {
      decoder->failure = true;
      return http_parsing::FAILURE;
    }

    decoder->request->url.query = std::move(decoded.get());

    Option<std::string> encoding =
      decoder->request->headers.get("Content-Encoding");

    if (encoding.isSome() && encoding.get() == "gzip") {
      decoder->decompressor =
        Owned<gzip::Decompressor>(new gzip::Decompressor());
    }

    CHECK_NONE(decoder->writer);

    http::Pipe pipe;
    decoder->writer = pipe.writer();
    decoder->request->reader = pipe.reader();

    // Send the request to the caller, but keep a Pipe::Writer for
    // streaming the body content into the request.
    decoder->requests.push_back(decoder->request);

    // TODO(alexr): We currently do not support trailers, i.e., extra headers
    // after `on_headers_complete` has been called. When we add trailer support,
    // we need a thread-safe way to surface them up in the request or some
    // auxiliary data structure.
    decoder->request = nullptr;

    return http_parsing::SUCCESS;
  }

  static int on_body(http_parser* p, const char* data, size_t length)
  {
    StreamingRequestDecoder* decoder = (StreamingRequestDecoder*) p->data;

    CHECK_SOME(decoder->writer);

    http::Pipe::Writer writer = decoder->writer.get(); // Remove const.

    std::string body;
    if (decoder->decompressor.get() != nullptr) {
      Try<std::string> decompressed =
        decoder->decompressor->decompress(std::string(data, length));

      if (decompressed.isError()) {
        decoder->failure = true;
        return http_parsing::FAILURE;
      }

      body = std::move(decompressed.get());
    } else {
      body = std::string(data, length);
    }

    writer.write(std::move(body));

    return http_parsing::SUCCESS;
  }

  static int on_message_complete(http_parser* p)
  {
    StreamingRequestDecoder* decoder = (StreamingRequestDecoder*) p->data;

    // This can happen if the callback `on_headers_complete()` had failed
    // earlier (e.g., due to invalid query parameters).
    if (decoder->writer.isNone()) {
      CHECK(decoder->failure);
      return http_parsing::FAILURE;
    }

    http::Pipe::Writer writer = decoder->writer.get(); // Remove const.

    if (decoder->decompressor.get() != nullptr &&
        !decoder->decompressor->finished()) {
      writer.fail("Failed to decompress body");
      decoder->failure = true;
      return http_parsing::FAILURE;
    }

    writer.close();

    decoder->writer = None();

    return http_parsing::SUCCESS;
  }

  bool failure;

  http_parser parser;
  http_parser_settings settings;

  enum
  {
    HEADER_FIELD,
    HEADER_VALUE
  } header;

  std::string field;
  std::string value;
  std::string query;
  std::string url;

  http::Request* request;
  Option<http::Pipe::Writer> writer;
  Owned<gzip::Decompressor> decompressor;

  std::deque<http::Request*> requests;
};

}  // namespace process {

#endif // __DECODER_HPP__
