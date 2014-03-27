/* TODO(benh): TCP_CORK!!!!! */
/* TODO(benh): Complete HttpParser & HttpMessage implementation. */
/* TODO(benh): Turn off Nagle (on TCP_NODELAY) for pipelined requests. */

#include <string.h>

#include <process.hpp>
#include <tuple.hpp>

#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

#include <arpa/inet.h>

#include <stout/os.hpp>

#include "net.hpp"

#include "http-parser/http_parser.h"

using std::cerr;
using std::cout;
using std::endl;
using std::runtime_error;
using std::string;
using std::map;

using process::tuple::Tuple;

#define malloc(bytes)                                               \
  ({ void *tmp; if ((tmp = malloc(bytes)) == NULL) abort(); tmp; })

#define realloc(address, bytes)                                     \
  ({ void *tmp; if ((tmp = realloc(address, bytes)) == NULL) abort(); tmp; })

#define HTTP_500                                                    \
  "HTTP/1.1 500 Internal Server Error\r\n\r\n"

#define HTTP_501                                                    \
  "HTTP/1.1 501 Not Implemented\r\n\r\n"

#define HTTP_404                                                    \
  "HTTP/1.1 404 Not Found\r\n\r\n"


struct HttpMessage
{
  unsigned short method;
  /* TODO(*): Use HTTP_MAX_URI_SIZE. */
  string uri;
};


class HttpParser
{
protected:
  static int on_uri(http_parser *parser, const char *p, size_t len)
  {
    HttpMessage *message = (HttpMessage *) parser->data;
    message->uri += string(p, len);
    return 0;
  }

  static int on_headers_complete(http_parser *parser)
  {
    HttpMessage *message = (HttpMessage *) parser->data;
    message->method = parser->method;
    return 0;
  }

public:
  static HttpMessage * parse(const string &raw)
  {
    http_parser parser;
    http_parser_init(&parser, HTTP_REQUEST);

    HttpMessage *message = new HttpMessage;

    parser.data = message;

    parser.on_message_begin     = NULL;
    parser.on_header_field      = NULL;
    parser.on_header_value      = NULL;
    parser.on_path              = NULL;
    parser.on_uri               = &HttpParser::on_uri;
    parser.on_fragment          = NULL;
    parser.on_query_string      = NULL;
    parser.on_body              = NULL;
    parser.on_headers_complete  = &HttpParser::on_headers_complete;
    parser.on_message_complete  = NULL;

    http_parser_execute(&parser, raw.c_str(), raw.length());
    
    if (http_parser_has_error(&parser)) {
      //cerr << "parser error" << endl;
      abort();
    }

    return message;
  }
};


class HttpConnection : public SocketProcess<TCP>
{
protected:
  void operator () ()
  {
    //cout << ht_id() << ": running " << this << " connection (1)" << endl;

    string raw;

    /* Read headers (until CRLF CRLF). */
    do {
      char buf[512];
      ssize_t len = recv(buf, 512);
      raw += string(buf, len);
    } while (raw.find("\r\n\r\n") == string::npos);

    //cout << ht_id() << ": running " << this << " connection (2)" << endl;

    /* Parse headers. */
    HttpMessage *message = HttpParser::parse(raw);

    /* Handle request. */
    switch (message->method) {
    case HTTP_GET: {
      message->uri =
        message->uri != "/"
        ? "." + message->uri
        : "./index.html";

      //cout << "URI: " << message->uri << endl;

      /* Open file (if possible). */
      int fd;

      if ((fd = open(message->uri.c_str(), O_RDONLY, 0)) < 0) {
        send(HTTP_404, strlen(HTTP_404));
        return;
      }

      /* Lookup file size. */
      struct stat fd_stat;

      if (fstat(fd, &fd_stat) < 0) {
        send(HTTP_500, strlen(HTTP_500));
        os::close(fd);
        return;
      }

      /* TODO(benh): Use TCP_CORK. */

      /* Transmit reply header. */
      std::stringstream out;

      out << "HTTP/1.1 200 OK\r\n";

      /* Determine the content type. */
      if (message->uri.find(".jpg") != string::npos) {
        out << "Content-Type: image/jpeg\r\n";
      } else if (message->uri.find(".gif") != string::npos) {
        out << "Content-Type: image/gif\r\n";
      } else if (message->uri.find(".png") != string::npos) {
        out << "Content-Type: image/png\r\n";
      } else if (message->uri.find(".css") != string::npos) {
        out << "Content-Type: text/css\r\n";
      } else {
        out << "Content-Type: text/html\r\n";
      }

      out <<
        "Content-Length: " << fd_stat.st_size << "\r\n"
        "\r\n";

      //cout << out.str() << endl;

      send(out.str().c_str(), out.str().size());

      //cout << ht_id() << ": running " << this << " connection (3)" << endl;

      /* Transmit file (TODO(benh): Use file cache.). */
      sendfile(fd, fd_stat.st_size);

      //cout << ht_id() << ": running " << this << " connection (4)" << endl;

      os::close(fd);

      break;
    }

    default:
      /* Unimplemented. */
      send(HTTP_501, strlen(HTTP_501));
      break;
    }
  }

public:
  explicit HttpConnection(int s) : SocketProcess<TCP>(s) {}
  ~HttpConnection() {}
};


enum HTTP_MESSAGES { ACCEPT = PROCESS_MSGID };

namespace process { namespace tuple { TUPLE(::ACCEPT, (int)); }}

class HttpAcceptor : public Tuple< Acceptor<TCP> >
{
private:
  PID server;

protected:
  void operator () ()
  {
    do {
      struct sockaddr_in addr;
      int c = accept(addr);
      //cout << ht_id() << ": running acceptor" << endl;
      send<ACCEPT>(server, c);
    } while (true);
  }

public:
  HttpAcceptor(const PID &_server, int s) : server(_server) { socket(s); }
};



class HttpServer : public Tuple< Server<TCP> >
{
private:
  map<PID, HttpConnection *> connections;

protected:
  void operator () ()
  {
    int on = 1;
    setsockopt(SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    bind();
    listen(100000);

    HttpAcceptor *acceptor = new HttpAcceptor(self(), s);
    link(spawn(acceptor));

    do {
      switch (receive()) {
      case ACCEPT: {
        //cout << ht_id() << ": running server (accept)" << endl;
        int c;
        unpack<ACCEPT>(c);
        HttpConnection *connection = new HttpConnection(c);
        connections[link(spawn(connection))] = connection;
        //cout << "...started (" << connection << ")..." << endl;
        break;
      }
      case PROCESS_EXIT: {
        //cout << ht_id() << ": running server (exit)" << endl;
        if (from() == acceptor->getPID()) {
          throw runtime_error("unimplemented acceptor failure");
        } else if (connections.find(from()) != connections.end()) {
          HttpConnection *connection = connections[from()];
          connections.erase(from());
          delete connection;
          //cout << "...finished (" << connection << ")..." << endl;
        }
        break;
      }
      default:
        cout << "HttpServer received unexpected message" << endl;
        break;
      }
    } while (true);
  }

public:
  HttpServer(int port) { init(INADDR_ANY, port); }
};



int main(int argc, char **argv)
{
  /* TODO(benh): Blah, 'sendfile' doesn't let us use MSG_NOSIGNAL. :(  */
  signal(SIGPIPE, SIG_IGN);

  if (argc != 2) {
    cerr << "usage: " << argv[0] << " <port>" << endl;
    return -1;
  }

#ifdef USE_LITHE
  ProcessScheduler *scheduler = new ProcessScheduler();
  Process::spawn(new HttpServer(atoi(argv[1])));
  /* TODO(benh): Make Process::wait take and use the hart if using Lithe! */
  for (;;)
    sleep(10000);
#else
  Process::wait(Process::spawn(new HttpServer(atoi(argv[1]))));
#endif /* USE_LITHE */

  return 0;
}
