#include <stdarg.h>
#include <string.h>

#include <process.hpp>

#include <string>

PID out;

enum { PRINT = PROCESS_MSGID, PRINTLN };

class Out : public Process
{
protected:
  void operator () ()
  {
    for (;;) {
      switch (receive()) {
      case PRINT: {
	const char *s = body(NULL);
	std::cout << body(NULL);
	break;
      }
      case PRINTLN: {
	std::cout << body(NULL) << std::endl;
	break;
      }
      default:
	std::cout << "'out' received unexpected message" << std::endl;
	break;
      }
    }
  }

public:
  static void print(const char *fmt, ...)
  {
    va_list args;
    va_start(args, fmt);
    char *s;
    vasprintf(&s, fmt, args);
    va_end(args);
    Process::post(out, PRINT, s, strlen(s) + 1);
    free(s);
  }

  static void println(const char *fmt, ...)
  {
    va_list args;
    va_start(args, fmt);
    char *s;
    vasprintf(&s, fmt, args);
    va_end(args);
    Process::post(out, PRINTLN, s, strlen(s) + 1);
    free(s);
  }
};

static void __attribute__ ((constructor)) init()
{
  out = Process::spawn(new Out());
}

// class In *in;

// class In : public Tuple<Process>
// {
// protected:
//   void operator () ()
//   {
//     for (;;) {
//       switch (receive()) {
//       case PRINT:
// 	string s;
// 	unpack(s);
// 	cout << s;
// 	break;
//       case PRINTLN:
// 	string s;
// 	unpack(s);
// 	cout << s << endl;
// 	break;
//       default:
// 	cout << "'out' received unexpected message" << endl;
// 	break;
//       }
//     }
//   }

// public:
//   static char read()
//   {
//     send<READ>(in);
//     receive();
//     assert(msgid() == OKAY);
//     char c;
//     unpack(c);
//     return c;
//   }

//   static std::string readln()
//   {
//     send<READLN>(in);
//     receive();
//     assert(msgid() == OKAY);
//     std::string s;
//     unpack(s);
//     return s;
//   }
// };
