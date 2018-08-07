// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <process/collect.hpp>
#include <process/count_down_latch.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/hashset.hpp>
#include <stout/stopwatch.hpp>

#include "benchmarks.pb.h"

#include "mpsc_linked_queue.hpp"

namespace http = process::http;
namespace metrics = process::metrics;

using process::CountDownLatch;
using process::Future;
using process::MessageEvent;
using process::Owned;
using process::Process;
using process::ProcessBase;
using process::Promise;
using process::UPID;

using std::cout;
using std::endl;
using std::ostringstream;
using std::string;
using std::vector;

using testing::WithParamInterface;

int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

  // NOTE: Windows does not support signal semantics required for these
  // handlers to be useful.
#ifndef __WINDOWS__
  // Install GLOG's signal handler.
  google::InstallFailureSignalHandler();

  // We reset the GLOG's signal handler for SIGTERM because
  // 'SubprocessTest.Status' sends SIGTERM to a subprocess which
  // results in a stack trace otherwise.
  os::signals::reset(SIGTERM);
#endif // __WINDOWS__

  // Add the libprocess test event listeners.
  ::testing::TestEventListeners& listeners =
    ::testing::UnitTest::GetInstance()->listeners();

  listeners.Append(process::ClockTestEventListener::instance());
  listeners.Append(process::FilterTestEventListener::instance());

  int result = RUN_ALL_TESTS();

  process::finalize(true);
  return result;
}

// TODO(jmlvanre): Factor out the client / server behavior so that we
// can make separate binaries for the client and server. This is
// useful to attach performance tools to them separately.

// A process that emulates the 'client' side of a ping pong game.
// An HTTP '/run' request performs a run and returns the time elapsed.
class ClientProcess : public Process<ClientProcess>
{
public:
  ClientProcess()
    : requests(0),
      responses(0),
      totalRequests(0),
      concurrency(0) {}

  ~ClientProcess() override {}

protected:
  void initialize() override
  {
    install("pong", &ClientProcess::pong);

    route("/run", None(), &ClientProcess::run);
  }

private:
  Future<http::Response> run(const http::Request& request)
  {
    if (duration.get() != nullptr) {
      return http::BadRequest("A run is already in progress");
    }

    hashmap<string, Option<string>> parameters {
      {"server", request.url.query.get("server")},
      {"messageSize", request.url.query.get("messageSize")},
      {"requests", request.url.query.get("requests")},
      {"concurrency", request.url.query.get("concurrency")},
    };

    // Ensure all parameters were provided.
    foreachpair (const string& parameter,
                 const Option<string>& value,
                 parameters) {
      if (value.isNone()) {
        return http::BadRequest("Missing '" + parameter + "' parameter");
      }
    }

    server = UPID(parameters["server"].get());
    link(server);

    Try<Bytes> messageSize = Bytes::parse(parameters["messageSize"].get());
    if (messageSize.isError()) {
      return http::BadRequest("Invalid 'messageSize': " + messageSize.error());
    }
    message = string(messageSize->bytes(), '1');

    Try<size_t> numify_ = numify<size_t>(parameters["requests"].get());
    if (numify_.isError()) {
      return http::BadRequest("Invalid 'requests': " + numify_.error());
    }
    totalRequests = numify_.get();

    numify_ = numify<size_t>(parameters["concurrency"].get());
    if (numify_.isError()) {
      return http::BadRequest("Invalid 'concurrency': " + numify_.error());
    }
    concurrency = numify_.get();

    if (concurrency > totalRequests) {
      concurrency = totalRequests;
    }

    return _run()
      .then([](const Duration& duration) -> Future<http::Response> {
        return http::OK(stringify(duration));
      });
  }

  Future<Duration> _run()
  {
    duration = Owned<Promise<Duration>>(new Promise<Duration>());

    watch.start();

    while (requests < concurrency) {
      send(server, "ping", message.c_str(), message.size());
      ++requests;
    }

    return duration->future();
  }

  void pong(const UPID& from, const string& body)
  {
    ++responses;

    if (responses == totalRequests) {
      duration->set(watch.elapsed());
      duration.reset();
    } else if (requests < totalRequests) {
      send(server, "ping", message.c_str(), message.size());
      ++requests;
    }
  }

  // The address of the ponger (server).
  UPID server;

  Stopwatch watch;

  Owned<Promise<Duration>> duration;

  string message;

  size_t requests;
  size_t responses;

  size_t totalRequests;
  size_t concurrency;
};


// A process that emulates the 'server' side of a ping pong game.
// Note that the server links to any clients communicating to it.
class ServerProcess : public Process<ServerProcess>
{
public:
  ~ServerProcess() override {}

protected:
  void initialize() override
  {
    // TODO(bmahler): Move in the message when move support is added.
    install("ping", &ServerProcess::ping);
  }

private:
  void ping(const UPID& from, const string& body)
  {
    if (!links.contains(from)) {
      link(from);
      links.insert(from);
    }

    send(from, "pong", body.c_str(), body.size());
  }

  hashset<UPID> links;
};

// TODO(bmahler): Since there is no forking here, libprocess
// avoids going through sockets for local messages. Either fork
// or have the ability to disable local messages in libprocess.

// Launches many clients against a central server and measures
// client throughput.
TEST(ProcessTest, Process_BENCHMARK_ClientServer)
{
  const size_t numRequests = 10000;
  const size_t concurrency = 250;
  const size_t numClients = 8;
  const Bytes messageSize = Bytes(3);

  ServerProcess server;
  const UPID serverPid = spawn(&server);

  // Launch the clients.
  vector<Owned<ClientProcess>> clients;
  for (size_t i = 0; i < numClients; i++) {
    clients.push_back(Owned<ClientProcess>(new ClientProcess()));
    spawn(clients.back().get());
  }

  // Start the ping / pongs!
  const string query = strings::join(
      "&",
      "server=" + stringify(serverPid),
      "requests=" + stringify(numRequests),
      "concurrency=" + stringify(concurrency),
      "messageSize=" + stringify(messageSize));

  Stopwatch watch;
  watch.start();

  vector<Future<http::Response>> futures;
  foreach (const Owned<ClientProcess>& client, clients) {
    futures.push_back(http::get(client->self(), "run", query));
  }

  Future<vector<http::Response>> responses = collect(futures);
  AWAIT_READY(responses);

  Duration elapsed = watch.elapsed();

  // Print the throughput of each client.
  size_t i = 0;
  foreach (const http::Response& response, responses.get()) {
    ASSERT_EQ(http::Status::OK, response.code);
    ASSERT_EQ(http::Status::string(http::Status::OK), response.status);

    Try<Duration> elapsed = Duration::parse(response.body);
    ASSERT_SOME(elapsed);
    double throughput = numRequests / elapsed->secs();

    cout << "Client " << i << ": " << throughput << " rpcs / sec" << endl;

    i++;
  }

  double throughput = (numRequests * numClients) / elapsed.secs();
  cout << "Estimated Total: " << throughput << " rpcs / sec" << endl;

  foreach (const Owned<ClientProcess>& client, clients) {
    terminate(*client);
    wait(*client);
  }

  terminate(server);
  wait(server);
}


class LinkerProcess : public Process<LinkerProcess>
{
public:
  LinkerProcess(const UPID& _to) : to(_to) {}

  void initialize() override
  {
    link(to);
  }

private:
  UPID to;
};


class EphemeralProcess : public Process<EphemeralProcess>
{
public:
  void terminate()
  {
    process::terminate(self());
  }
};


// Simulate the scenario discussed in MESOS-2182. We first establish a
// large number of links by creating many linker-linkee pairs. And
// then, we introduce a large amount of ephemeral process exits as
// well as event dispatches.
TEST(ProcessTest, Process_BENCHMARK_LargeNumberOfLinks)
{
  int links = 5000;
  int iterations = 10000;

  // Keep track of all the linked processes we created.
  vector<ProcessBase*> processes;

  // Establish a large number of links.
  for (int i = 0; i < links; i++) {
    ProcessBase* linkee = new ProcessBase();
    LinkerProcess* linker = new LinkerProcess(linkee->self());

    processes.push_back(linkee);
    processes.push_back(linker);

    spawn(linkee);
    spawn(linker);
  }

  // Generate large number of dispatches and process exits by spawning
  // and then terminating EphemeralProcesses.
  vector<ProcessBase*> ephemeralProcesses;

  Stopwatch watch;
  watch.start();

  for (int i = 0; i < iterations ; i++) {
    EphemeralProcess* process = new EphemeralProcess();
    ephemeralProcesses.push_back(process);

    spawn(process);

    // NOTE: We let EphemeralProcess terminate itself to make sure all
    // dispatches are actually executed (otherwise, 'wait' below will
    // be blocked).
    dispatch(process->self(), &EphemeralProcess::terminate);
  }

  foreach (ProcessBase* process, ephemeralProcesses) {
    wait(process);
    delete process;
  }

  cout << "Elapsed: " << watch.elapsed() << endl;

  foreach (ProcessBase* process, processes) {
    terminate(process);
    wait(process);
    delete process;
  }
}


class Destination : public Process<Destination>
{
protected:
  void consume(MessageEvent&& event) override
  {
    if (event.message.name == "ping") {
      send(event.message.from, "pong");
    }
  }
};


class Client : public Process<Client>
{
public:
  Client(const UPID& destination, CountDownLatch* latch, long repeat)
    : destination(destination), latch(latch), repeat(repeat) {}

protected:
  void consume(MessageEvent&& event) override
  {
    if (event.message.name == "pong") {
      received += 1;
      if (sent < repeat) {
        send(destination, "ping");
        sent += 1;
      } else if (received >= repeat) {
        latch->decrement();
      }
    } else if (event.message.name == "run") {
      for (long l = 0; l < std::min(1000L, repeat); l++) {
        send(destination, "ping");
        sent += 1;
      }
    }
  }

private:
  UPID destination;
  CountDownLatch* latch;
  long repeat;
  long sent = 0L;
  long received = 0L;
};


// See
// https://github.com/akka/akka/blob/7ac37e7536547c57ab639ed8746c7b4e5ff2f69b/akka-actor-tests/src/test/scala/akka/performance/microbench/TellThroughputPerformanceSpec.scala
// for the inspiration for this benchmark (this file was deleted in
// this commit:
// https://github.com/akka/akka/commit/a02e138f3bc7c21c2b2511ea19203a52d74584d5).
//
// This benchmark was discussed here:
// http://letitcrash.com/post/17607272336/scalability-of-fork-join-pool
TEST(ProcessTest, Process_BENCHMARK_ThroughputPerformance)
{
  long repeatFactor = 500L;
  long defaultRepeat = 30000L * repeatFactor;

  const long numberOfClients = process::workers();

  CountDownLatch latch(numberOfClients - 1);

  long repeat = defaultRepeat;

  auto repeatsPerClient = repeat / numberOfClients;

  vector<Owned<Destination>> destinations;
  vector<Owned<Client>> clients;

  for (long _ = 0; _ < numberOfClients; _++) {
    Owned<Destination> destination(new Destination());

    spawn(*destination);

    Owned<Client> client(new Client(
        destination->self(),
        &latch,
        repeatsPerClient));

    spawn(*client);

    destinations.push_back(destination);
    clients.push_back(client);
  }

  Stopwatch watch;
  watch.start();

  foreach (const Owned<Client>& client, clients) {
    post(client->self(), "run");
  }

  AWAIT_READY(latch.triggered());

  Duration elapsed = watch.elapsed();

  double throughput = (double) repeat / elapsed.secs();

  cout << "Estimated Total: " << std::fixed << throughput << endl;

  foreach (const Owned<Client>& client, clients) {
    terminate(client->self());
    wait(client->self());
  }

  foreach (const Owned<Destination>& destination, destinations) {
    terminate(destination->self());
    wait(destination->self());
  }
}


class DispatchProcess : public Process<DispatchProcess>
{
public:
  struct Movable
  {
    std::vector<int> data;
  };

  // This simulates protobuf objects, which do not support moves.
  struct Copyable
  {
    std::vector<int> data;

    Copyable(std::vector<int>&& data) : data(std::move(data)) {}
    Copyable(const Copyable& that) = default;
    Copyable& operator=(const Copyable&) = default;
  };

  DispatchProcess(Promise<Nothing> *promise, long repeat)
    : promise(promise), repeat(repeat) {}

  template <typename T>
  Future<Nothing> handler(const T& data)
  {
    count++;
    if (count >= repeat) {
      promise->set(Nothing());
      return Nothing();
    }

    // NOTE: The prefix `this->` is required here, otherwise it will
    // not compile when permissiveness is disabled (e.g. with MSVC on
    // Windows).
    dispatch(this->self(), &Self::_handler).then(
        defer(this->self(), &Self::handler<T>, data));

    return Nothing();
  }

  template <typename T>
  static void run(const string& name, long repeats)
  {
    Promise<Nothing> promise;

    Owned<DispatchProcess> process(new DispatchProcess(&promise, repeats));
    spawn(*process);

    T data{std::vector<int>(10240, 42)};

    Stopwatch watch;
    watch.start();

    dispatch(process.get(), &DispatchProcess::handler<T>, data);

    AWAIT_READY(promise.future());

    cout << name << " elapsed: " << watch.elapsed() << endl;

    terminate(process.get());
    wait(process.get());
  }

private:
  Future<Nothing> _handler()
  {
    return Nothing();
  }

  Promise<Nothing> *promise;
  long repeat;
  long count = 0;
};

TEST(ProcessTest, Process_BENCHMARK_DispatchDefer)
{
  constexpr long repeats = 100000;

  // Test performance separately for objects which support std::move,
  // and which don't (e.g. like protobufs).
  // Note: DispatchProcess::handler code is not fully optimized,
  // to take advantage of std::move support, e.g. parameter is passed
  // by const reference, so some copying is unavoidable, however
  // this resembles how most of the handlers are currently implemented.
  DispatchProcess::run<DispatchProcess::Movable>("Movable", repeats);
  DispatchProcess::run<DispatchProcess::Copyable>("Copyable", repeats);
}


class ProtobufInstallHandlerBenchmarkProcess
  : public ProtobufProcess<ProtobufInstallHandlerBenchmarkProcess>
{
public:
  ProtobufInstallHandlerBenchmarkProcess()
  {
    install<tests::Message>(&Self::handle);
  }

  // TODO(dzhuk): Add benchmark for handlers taking individual
  // message fields as parameters.
  void handle(const tests::Message& message)
  {
    // Intentionally no-op, as ProtobufProcess performance is measured
    // from receiving MessageEvent till calling handler.
  }

  void run(int submessages)
  {
    tests::Message message = createMessage(submessages);

    std::string data;
    bool success = message.SerializeToString(&data);
    CHECK(success);

    Stopwatch watch;
    watch.start();

    size_t count;

    for (count = 0; watch.elapsed() < Seconds(1); count++) {
      MessageEvent event(self(), self(), message.GetTypeName(),
          data.c_str(), data.length());
      consume(std::move(event));
    }

    watch.stop();

    double messagesPerSecond = count / watch.elapsed().secs();

    cout << "Size: " << std::setw(5) << message.ByteSizeLong() << " bytes,"
         << " throughput: " << std::setw(9) << std::setprecision(0)
         << std::fixed << messagesPerSecond << " messages/s" << endl;
  }

private:
  // Returns a tree with the `submessages` number of sub-messages,
  // the branching factor is 4 and each sub-message contains a
  // payload of two integers. E.g.
  //
  //                             m                            |
  //        /           /        |        \         \         |
  //     [1,1]         m         m         m         m        |
  //                 //|\\     //|\\     //|\\     //|\\      |
  //               [1,1]...  [1,1]...  [1,1]...  [1,1]...     |
  tests::Message createMessage(size_t submessages)
  {
    tests::Message root;

    // Construct messages tree level by level, similar to breadth-first
    // search, where `submessages` defines the total number of nodes in
    // the tree. Messages in the queue still need a payload and children
    // to be added.
    std::deque<tests::Message*> nodes;
    nodes.push_back(&root);

    while (!nodes.empty()) {
      tests::Message* message = nodes.front();
      nodes.pop_front();

      message->mutable_payload()->Resize(2, 1);

      for (size_t i = 0; i < 4; i++) {
        if (submessages == 0) {
          // No more nodes need to be added, but keep processing the
          // queue to add the payloads.
          break;
        }

        tests::Message* child = message->add_submessages();
        nodes.push_back(child);
        submessages--;
      }
    }

    return root;
  }
};


// Measures performance of message passing in ProtobufProcess.
TEST(ProcessTest, Process_BENCHMARK_ProtobufInstallHandler)
{
  const int submessages[] = {0, 1, 5, 10, 50, 100, 500, 1000, 5000, 10000};

  ProtobufInstallHandlerBenchmarkProcess process;
  foreach (int num_submessages, submessages) {
    process.run(num_submessages);
  }
}


TEST(ProcessTest, Process_BENCHMARK_MpscLinkedQueue)
{
  // NOTE: we set the total number of producers to be 1 less than the
  // hardware concurrency so the consumer doesn't have to fight for
  // processing time with the producers.
  const unsigned int producerCount = std::thread::hardware_concurrency() - 1;
  const int messageCount = 10000000;
  const int totalCount = messageCount * producerCount;
  std::string* s = new std::string("");
  process::MpscLinkedQueue<std::string> q;

  Stopwatch consumerWatch;

  auto consumer = std::thread([totalCount, &q, &consumerWatch]() {
    consumerWatch.start();
    for (int i = totalCount; i > 0;) {
      if (q.dequeue() != nullptr) {
        i--;
      }
    }
    consumerWatch.stop();
  });

  std::vector<std::thread> producers;

  Stopwatch producerWatch;
  producerWatch.start();

  for (unsigned int t = 0; t < producerCount; t++) {
    // We want to capture `messageCount`, `s`, and `&q` here. Since
    // `messageCount` is a constant integer variable initialized with a
    // compile-time expression in a "reaching scope", it can get captured
    // without being mentioned in the capture list, see. e.g.,
    // https://stackoverflow.com/a/43468519/176922.
    //
    // We capture implicitly instead of explicitly since this part of the
    // standard is not supported by msvc, while clang supports it and emits a
    // warning for unneeded captures.
    producers.push_back(std::thread([&]() {
      for (int i = 0; i < messageCount; i++) {
        q.enqueue(s);
      }
    }));
  }

  for (std::thread& producer : producers) {
    producer.join();
  }

  producerWatch.stop();

  consumer.join();

  Duration producerElapsed = producerWatch.elapsed();
  Duration consumerElapsed = consumerWatch.elapsed();

  double consumerThroughput = (double) totalCount / consumerElapsed.secs();
  double producerThroughput = (double) totalCount / producerElapsed.secs();
  double throughput = consumerThroughput + producerThroughput;

  cout << "Estimated producer throughput (" << producerCount << " threads): "
       << std::fixed << producerThroughput << " op/s" << endl;
  cout << "Estimated consumer throughput: "
       << std::fixed << consumerThroughput << " op/s" << endl;
  cout << "Estimated total throughput: "
       << std::fixed << throughput << " op/s" << endl;
}


class Metrics_BENCHMARK_Test : public ::testing::Test,
                               public WithParamInterface<size_t>{};


// Parameterized by the number of metrics.
INSTANTIATE_TEST_CASE_P(
    MetricsCount,
    Metrics_BENCHMARK_Test,
    ::testing::Values(1u, 100u, 1000u, 10000u, 100000u));


// Tests the performance of metrics fetching when there
// are a large number of metrics.
TEST_P(Metrics_BENCHMARK_Test, Scalability)
{
  size_t metrics_count = GetParam();

  vector<metrics::Counter> counters;
  counters.reserve(metrics_count);

  for (size_t i = 0; i < metrics_count; ++i) {
    counters.push_back(
        metrics::Counter("metrics/keys/can/be/somewhat/long/"
                         "so/we/use/a/fairly/long/key/here/"
                         "to/test/a/more/pathological/case/" +
                         stringify(i)));
  }

  Stopwatch watch;
  watch.start();
  for (size_t i = 0; i < metrics_count; ++i) {
    metrics::add(counters[i]).get();
  }
  watch.stop();

  std::cout << "Added " << metrics_count << " counters in "
            << watch.elapsed() << std::endl;

  watch.start();
  metrics::snapshot(None()).get();
  watch.stop();

  std::cout << "Snapshot of " << metrics_count << " counters in "
              << watch.elapsed() << std::endl;

  UPID upid("metrics", process::address());

  watch.start();
  http::get(upid, "snapshot").get();
  watch.stop();

  std::cout << "HTTP /snapshot of " << metrics_count << " counters in "
              << watch.elapsed() << std::endl;

  watch.start();
  for (size_t i = 0; i < metrics_count; ++i) {
    metrics::remove(counters[i]).get();
  }
  watch.stop();

  std::cout << "Removed " << metrics_count << " counters in "
            << watch.elapsed() << std::endl;
}


TEST(ProcessTest, Process_BENCHMARK_MpscLinkedQueueEmpty)
{
  const int messageCount = 1000000000;
  process::MpscLinkedQueue<std::string> q;

  Stopwatch consumerWatch;
  consumerWatch.start();

  for (int i = messageCount; i > 0; i--) {
    q.dequeue();
  }

  consumerWatch.stop();

  Duration consumerElapsed = consumerWatch.elapsed();

  double consumerThroughput = messageCount / consumerElapsed.secs();

  cout << "Estimated consumer throughput: "
       << std::fixed << consumerThroughput << " op/s" << endl;
}


TEST(ProcessTest, Process_BENCHMARK_MpscLinkedQueueNonContendedRead)
{
  // NOTE: we set the total number of producers to be 1 less than the
  // hardware concurrency so the consumer doesn't have to fight for
  // processing time with the producers.
  const unsigned int producerCount = std::thread::hardware_concurrency() - 1;
  const int messageCount = 10000000;
  const int totalCount = messageCount * producerCount;
  std::string* s = new std::string("");
  process::MpscLinkedQueue<std::string> q;

  std::vector<std::thread> producers;
  for (unsigned int t = 0; t < producerCount; t++) {
    producers.push_back(std::thread([&]() {
      for (int i = 0; i < messageCount; i++) {
        q.enqueue(s);
      }
    }));
  }

  Stopwatch producerWatch;
  producerWatch.start();

  for (std::thread& producer : producers) {
    producer.join();
  }

  producerWatch.stop();

  Stopwatch consumerWatch;
  consumerWatch.start();

  for (int i = totalCount; i > 0;) {
    if (q.dequeue() != nullptr) {
      i--;
    }
  }

  consumerWatch.stop();

  Duration producerElapsed = producerWatch.elapsed();
  Duration consumerElapsed = consumerWatch.elapsed();

  double consumerThroughput = totalCount / consumerElapsed.secs();
  double producerThroughput = totalCount / producerElapsed.secs();
  double throughput = consumerThroughput + producerThroughput;

  cout << "Estimated producer throughput (" << producerCount << " threads): "
       << std::fixed << producerThroughput << " op/s" << endl;
  cout << "Estimated consumer throughput: "
       << std::fixed << consumerThroughput << " op/s" << endl;
  cout << "Estimated total throughput: "
       << std::fixed << throughput << " op/s" << endl;
}
