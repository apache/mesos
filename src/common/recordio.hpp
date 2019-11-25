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

#ifndef __COMMON_RECORDIO_HPP__
#define __COMMON_RECORDIO_HPP__

#include <queue>
#include <string>
#include <utility>

#include <mesos/mesos.hpp>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/http.hpp>
#include <process/loop.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/recordio.hpp>
#include <stout/result.hpp>

namespace mesos {
namespace internal {
namespace recordio {

namespace internal {
template <typename T>
class ReaderProcess;
} // namespace internal {


/**
 * Provides RecordIO decoding on top of an http::Pipe::Reader.
 * The caller is responsible for closing the http::Pipe::Reader
 * when a failure is encountered or end-of-file is reached.
 *
 * TODO(bmahler): Since we currently do not have a generalized
 * abstraction in libprocess for "streams" of asynchronous data
 * (e.g. process::Stream<T>), we have to create a one-off wrapper
 * here. In the future, this would be better expressed as "piping"
 * data from a stream of raw bytes into a decoder, which yields a
 * stream of typed data.
 */
template <typename T>
class Reader
{
public:
  // We spawn `ReaderProcess` as a managed process to guarantee
  // that it does not wait on itself (this would cause a deadlock!).
  // See comments in `Connection::Data` for further details.
  Reader(std::function<Try<T>(const std::string&)> deserialize,
         process::http::Pipe::Reader reader)
    : process(process::spawn(
        new internal::ReaderProcess<T>(std::move(deserialize), reader),
        true)) {}

  virtual ~Reader()
  {
    // Note that we pass 'false' here to avoid injecting the
    // termination event at the front of the queue. This is
    // to ensure we don't drop any queued request dispatches
    // which would leave the caller with a future stuck in
    // a pending state.
    process::terminate(process, false);
  }

  /**
   * Returns the next piece of decoded data from the pipe.
   * Returns error if an individual record could not be decoded.
   * Returns none when end-of-file is reached.
   * Returns failure when the pipe or decoder has failed.
   */
  process::Future<Result<T>> read()
  {
    return process::dispatch(process, &internal::ReaderProcess<T>::read);
  }

private:
  process::PID<internal::ReaderProcess<T>> process;
};


/**
 * This is a helper function that reads records from a `Reader`, applies
 * a transformation to the records and writes to the pipe.
 *
 * Returns a failed future if there are any errors reading or writing.
 * The future is satisfied when we get a EOF.
 *
 * TODO(vinod): Split this method into primitives that can transform a
 * stream of bytes to a stream of typed records that can be further transformed.
 * See the TODO above in `Reader` for further details.
 */
template <typename T>
process::Future<Nothing> transform(
    process::Owned<Reader<T>>&& reader,
    const std::function<std::string(const T&)>& func,
    process::http::Pipe::Writer writer)
{
  return process::loop(
      None(),
      [=]() {
        return reader->read();
      },
      [=](const Result<T>& record) mutable
        -> process::Future<process::ControlFlow<Nothing>> {
        // This could happen if EOF is sent by the writer.
        if (record.isNone()) {
          return process::Break();
        }

        // This could happen if there is a de-serialization error.
        if (record.isError()) {
          return process::Failure(record.error());
        }

        // TODO(vinod): Instead of detecting that the reader went away only
        // after attempting a write, leverage `writer.readerClosed` future.
        if (!writer.write(func(record.get()))) {
          return process::Failure("Write failed to the pipe");
        }

        return process::Continue();
      });
}


namespace internal {

template <typename T>
class ReaderProcess : public process::Process<ReaderProcess<T>>
{
public:
  ReaderProcess(
      std::function<Try<T>(const std::string&)>&& _deserialize,
      process::http::Pipe::Reader _reader)
    : process::ProcessBase(process::ID::generate("__reader__")),
      deserialize(_deserialize),
      reader(_reader),
      done(false) {}

  ~ReaderProcess() override {}

  process::Future<Result<T>> read()
  {
    if (!records.empty()) {
      Result<T> record = std::move(records.front());
      records.pop();
      return record;
    }

    if (error.isSome()) {
      return process::Failure(error->message);
    }

    if (done) {
      return None();
    }

    auto waiter = process::Owned<process::Promise<Result<T>>>(
        new process::Promise<Result<T>>());
    waiters.push(std::move(waiter));
    return waiters.back()->future();
  }

protected:
  void initialize() override
  {
    consume();
  }

  void finalize() override
  {
    // Fail any remaining waiters.
    fail("Reader is terminating");
  }

private:
  void fail(const std::string& message)
  {
    error = Error(message);

    while (!waiters.empty()) {
      waiters.front()->fail(message);
      waiters.pop();
    }
  }

  void complete()
  {
    done = true;

    while (!waiters.empty()) {
      waiters.front()->set(Result<T>::none());
      waiters.pop();
    }
  }

  using process::Process<ReaderProcess<T>>::consume;

  void consume()
  {
    reader.read()
      .onAny(process::defer(this, &ReaderProcess::_consume, lambda::_1));
  }

  void _consume(const process::Future<std::string>& read)
  {
    if (!read.isReady()) {
      fail("Pipe::Reader failure: " +
           (read.isFailed() ? read.failure() : "discarded"));
      return;
    }

    // Have we reached EOF?
    if (read->empty()) {
      complete();
      return;
    }

    Try<std::deque<std::string>> decode = decoder.decode(read.get());

    if (decode.isError()) {
      fail("Decoder failure: " + decode.error());
      return;
    }

    foreach (const std::string& record, decode.get()) {
      Result<T> t = deserialize(record);

      if (!waiters.empty()) {
        waiters.front()->set(std::move(t));
        waiters.pop();
      } else {
        records.push(std::move(t));
      }
    }

    consume();
  }

  ::recordio::Decoder decoder;
  std::function<Try<T>(const std::string&)> deserialize;
  process::http::Pipe::Reader reader;

  std::queue<process::Owned<process::Promise<Result<T>>>> waiters;
  std::queue<Result<T>> records;

  bool done;
  Option<Error> error;
};

} // namespace internal {
} // namespace recordio {
} // namespace internal {
} // namespace mesos {

#endif // __COMMON_RECORDIO_HPP__
