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

#include <iostream>
#include <string>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/strings.hpp>

using namespace process;

using namespace process::http;

using std::string;

class MyProcess : public Process<MyProcess>
{
public:
  MyProcess(): ProcessBase("my-process") {}
  ~MyProcess() override {}

  Future<int> func1()
  {
    promise.future().onAny(
        defer([=](const Future<int>& future) {
          terminate(self());
        }));
    return promise.future();
  }

  void func2(int i)
  {
    promise.set(i);
  }

  Future<Response> vars(const Request& request)
  {
    // Response response;
    // response.code = Status::OK;
    // response.headers["Content-Type"] = "text/plain";
    // response.headers["Content-Length"] = stringify(body.size());
    // response.type = Response::BODY;
    // response.body = body;

    string body = "... vars here ...";
    return OK(body);
  }

  void stop(const UPID& from, const string& body)
  {
    terminate(self());
  }

protected:
  void initialize() override
  {
    // route("/vars", None(), &MyProcess::vars);
    route("/vars", None(), [=](const Request& request) {
      string body = "... vars here ...";
      return OK(body);
    });

    // install("stop", &MyProcess::stop);
    install("stop", [=](const UPID& from, const string& body) {
      terminate(self());
    });
  }

private:
  Promise<int> promise;
};


int main(int argc, char** argv)
{
  MyProcess process;
  PID<MyProcess> pid = spawn(&process);

  //// --------------------------------------

  // Future<int> future = dispatch(pid, &MyProcess::func1);
  // dispatch(pid, &MyProcess::func2, 42);

  // std::cout << future.get() << std::endl;

  // http::post(pid, "stop");

  //// --------------------------------------

  // Promise<bool> p;

  // dispatch(pid, &MyProcess::func1)
  //   .then([=, &p] (int i) {
  //       p.set(i == 42);
  //       return p.future();
  //     })
  //   .then([=] (bool b) {
  //       if (b) {
  //         http::post(pid, "stop");
  //       }
  //       return true; // No Future<void>.
  //     });

  // dispatch(pid, &MyProcess::func2, 42);

  //// --------------------------------------

  // dispatch(pid, &MyProcess::func1);
  // dispatch(pid, &MyProcess::func2, 42);

  //// --------------------------------------

  std::cout << strings::format("Endpoint listening on http://%s/%s/vars\n",
      process::address(),
      process.self().id).get();

  wait(pid);
  return 0;
}
