/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <iostream>
#include <memory>
#include <unordered_set>
#include <vector>

#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/process.hpp>

#include <stout/stopwatch.hpp>

using namespace process;

using std::cout;
using std::endl;
using std::function;
using std::istringstream;
using std::ostringstream;
using std::string;
using std::unique_ptr;
using std::unordered_set;
using std::vector;

int main(int argc, char** argv)
{
  // Initialize Google Mock/Test.
  testing::InitGoogleMock(&argc, argv);

  // Add the libprocess test event listeners.
  ::testing::TestEventListeners& listeners =
    ::testing::UnitTest::GetInstance()->listeners();

  listeners.Append(process::ClockTestEventListener::instance());
  listeners.Append(process::FilterTestEventListener::instance());

  return RUN_ALL_TESTS();
}


class BenchmarkProcess : public Process<BenchmarkProcess>
{
public:
  BenchmarkProcess(
      int _iterations = 1,
      int _maxOutstanding = 1,
      const Option<UPID>& _other = Option<UPID>())
    : other(_other),
      counter(0),
      iterations(_iterations),
      maxOutstanding(_maxOutstanding),
      outstanding(0),
      sent(0)
  {
    if (other.isSome()) {
      setLink(other.get());
    }
  }

  virtual ~BenchmarkProcess() {}

  virtual void initialize()
  {
    install("ping", &BenchmarkProcess::ping);
    install("pong", &BenchmarkProcess::pong);
  }

  void setLink(const UPID& that)
  {
    link(that);
  }

  void start()
  {
    watch.start();
    sendRemaining();
  }

  // Returns the number of rpcs performed per second.
  int await()
  {
    latch.await();
    double elapsed = watch.elapsed().secs();
    return iterations / elapsed;
  }

private:
  void ping(const UPID& from, const string& body)
  {
    if (linkedPorts.find(from.address.port) == linkedPorts.end()) {
      setLink(from);
      linkedPorts.insert(from.address.port);
    }
    static const string message("hi");
    send(from, "pong", message.c_str(), message.size());
  }

  void pong(const UPID& from, const string& body)
  {
    ++counter;
    --outstanding;
    if (counter >= iterations) {
      latch.trigger();
      watch.stop();
    }
    sendRemaining();
  }

  void sendRemaining()
  {
    static const string message("hi");
    for (; outstanding < maxOutstanding && sent < iterations;
         ++outstanding, ++sent) {
      send(other.get(), "ping", message.c_str(), message.size());
    }
  }

  Option<UPID> other;

  Latch latch;
  Stopwatch watch;

  int counter;

  const int iterations;
  const int maxOutstanding;
  int outstanding;
  int sent;
  unordered_set<int> linkedPorts;
};


typedef int pipes[2];


void createPipes(pipes& _pipes)
{
  if (pipe(_pipes) < 0) {
    perror("Pipe failed");
    abort();
  }
  Try<Nothing> cloexec = os::cloexec(_pipes[0]);
  if (cloexec.isError()) {
    perror("Cloexec failed on pipe");
    abort();
  }
  cloexec = os::cloexec(_pipes[1]);
  if (cloexec.isError()) {
    perror("Cloexec failed on pipe");
    abort();
  }
}


// Launch numberOfProcesses processes, each with clients 'client'
// Actors. Play ping pong back and forth between these actors and the
// main 'server' actor. Each 'client' can have queueDepth ping
// requests outstanding to the 'server' actor.
TEST(Process, Process_BENCHMARK_Test)
{
  const int iterations = 2500;
  const int queueDepth = 250;
  const int clients = 8;
  const int numberOfProcesses = 4;

  vector<int> outPipes;
  vector<int> inPipes;
  vector<pid_t> pids;
  for (int moreToLaunch = numberOfProcesses;
       moreToLaunch > 0; --moreToLaunch) {
    // fork in order to get numberOfProcesses seperate
    // ProcessManagers. This avoids the short-circuit built into
    // ProcessManager for processes communicating in the same manager.
    int requestPipes[2];
    int resultPipes[2];
    pid_t pid = -1;
    createPipes(requestPipes);
    createPipes(resultPipes);
    pid = fork();

    if (pid < 0) {
      perror("fork() failed");
      abort();
    } else if (pid == 0) {
      // Child.

      // Read the number of bytes about to be parsed.
      int stringSize = 0;
      ssize_t result = read(requestPipes[0], &stringSize, sizeof(stringSize));
      EXPECT_EQ(result, sizeof(stringSize));
      char buffer[stringSize + 1];
      memset(&buffer, 0, stringSize + 1);

      // Read in the upid of the 'server' actor.
      result = read(requestPipes[0], &buffer, stringSize);
      EXPECT_EQ(result, stringSize);
      istringstream inStream(buffer);
      UPID other;
      inStream >> other;

      // Launch a thread for each client that backs an actor.
      vector<unique_ptr<BenchmarkProcess>> benchmarkProcesses;
      for (int i = 0; i < clients; ++i) {
        BenchmarkProcess* process = new BenchmarkProcess(
            iterations,
            queueDepth,
            other);
        benchmarkProcesses.push_back(unique_ptr<BenchmarkProcess>(process));
        spawn(process);
        process->start();
      }

      // Compute the total rpcs per second for this process, write the
      // computation back to the server end of the fork.
      int totalRpcPerSecond = 0;
      foreach (const auto& process, benchmarkProcesses) {
        int rpcPerSecond = process->await();
        totalRpcPerSecond += rpcPerSecond;
        terminate(*process);
        wait(*process);
      }

      result = write(
          resultPipes[1],
          &totalRpcPerSecond,
          sizeof(totalRpcPerSecond));
      EXPECT_EQ(result, sizeof(totalRpcPerSecond));
      close(requestPipes[0]);
      exit(0);
    } else {
      // Parent.

      // Keep track of the pipes to the child forks. This way the
      // results of their rpc / sec computations can be read back and
      // aggregated.
      outPipes.push_back(requestPipes[1]);
      inPipes.push_back(resultPipes[0]);
      pids.push_back(pid);

      // If this is the last child launched, then let the parent
      // become the 'server' actor.
      if (moreToLaunch == 1) {
        BenchmarkProcess process(iterations, queueDepth);
        const UPID pid = spawn(&process);

        // Stringify the server pid to send to the child processes.
        ostringstream outStream;
        outStream << pid;
        int stringSize = outStream.str().size();

        // For each child, write the size of the stringified pid as
        // well as the stringified pid to the pipe.
        foreach (int fd, outPipes) {
          ssize_t result = write(fd, &stringSize, sizeof(stringSize));
          EXPECT_EQ(result, sizeof(stringSize));
          result = write(fd, outStream.str().c_str(), stringSize);
          EXPECT_EQ(result, stringSize);
          close(fd);
        }

        // Read the resulting rpcs / second from the child processes
        // and aggregate the results.
        int totalRpcsPerSecond = 0;
        foreach (int fd, inPipes) {
          int rpcs = 0;
          ssize_t result = read(fd, &rpcs, sizeof(rpcs));
          EXPECT_EQ(result, sizeof(rpcs));
          if (result != sizeof(rpcs)) {
            abort();
          }
          totalRpcsPerSecond += rpcs;
        }

        // Wait for all the child forks to terminately gracefully.
        foreach (const auto& p, pids) {
          ::waitpid(p, NULL, 0);
        }
        printf("Total: [%d] rpcs / s\n", totalRpcsPerSecond);
        terminate(process);
        wait(process);
      }
    }
  }
}


class LinkerProcess : public Process<LinkerProcess>
{
public:
  LinkerProcess(const UPID& _to) : to(_to) {}

  virtual void initialize()
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
TEST(Process, Process_BENCHMARK_LargeNumberOfLinks)
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
