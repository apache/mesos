#include <iostream>
#include <sstream>

#include <process.hpp>


using process::Future;
using process::HttpOKResponse;
using process::HttpRequest;
using process::HttpResponse;
using process::PID;
using process::Process;
using process::Promise;


class MyProcess : public Process<MyProcess>
{
public:
  MyProcess()
  {
    std::cout << "MyProcess at " << self() << std::endl;

    install("ehlo", &MyProcess::ehlo);
    install("vars", &MyProcess::vars);
  }

  Promise<int> func1()
  {
    std::cout << "MyProcess::func1 (this = " << this << ")" << std::endl;
    return promise;
  }

  void func2(int i)
  {
    std::cout << "MyProcess::func2 (this = " << this << ")" << std::endl;
    promise.set(i);
  }

  void ehlo()
  {
    std::cout << "MyProcess::ehlo (this = " << this << ")" << std::endl;
    std::cout << "from(): " << from() << std::endl;
    std::cout << "name(): " << name() << std::endl;
    std::cout << "body(): " << body() << std::endl;
  }

  Promise<HttpResponse> vars(const HttpRequest& request)
  {
    std::cout << "MyProcess::vars (this = " << this << ")" << std::endl;
    std::string body = "... vars here ...";
    HttpOKResponse response;
    response.headers["Content-Type"] = "text/plain";
    std::ostringstream out;
    out << body.size();
    response.headers["Content-Length"] = out.str();
    response.body = body;
    return response;
  }

private:
  Promise<int> promise;
};


class Foo : public Process<Foo> {};


int main(int argc, char** argv)
{
  MyProcess process;
  PID<MyProcess> pid = process::spawn(&process);

  Future<int> future = process::dispatch(pid, &MyProcess::func1);
  process::dispatch(pid, &MyProcess::func2, 42);

  std::cout << future.get() << std::endl;

  process::post(pid, "ehlo");

  process::wait(pid);
  return 0;
}
