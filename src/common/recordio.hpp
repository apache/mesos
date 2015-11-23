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
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/lambda.hpp>
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
  Reader(::recordio::Decoder<T>&& decoder,
         process::http::Pipe::Reader reader)
    : process(new internal::ReaderProcess<T>(std::move(decoder), reader))
  {
    process::spawn(process.get());
  }

  virtual ~Reader()
  {
    process::terminate(process.get());
    process::wait(process.get());
  }

  /**
   * Returns the next piece of decoded data from the pipe.
   * Returns error if an individual record could not be decoded.
   * Returns none when end-of-file is reached.
   * Returns failure when the pipe or decoder has failed.
   */
  process::Future<Result<T>> read()
  {
    return process::dispatch(process.get(), &internal::ReaderProcess<T>::read);
  }

private:
  process::Owned<internal::ReaderProcess<T>> process;
};


namespace internal {

template <typename T>
class ReaderProcess : public process::Process<ReaderProcess<T>>
{
public:
  ReaderProcess(
      ::recordio::Decoder<T>&& _decoder,
      process::http::Pipe::Reader _reader)
    : decoder(_decoder),
      reader(_reader),
      done(false) {}

  virtual ~ReaderProcess() {}

  process::Future<Result<T>> read()
  {
    if (!records.empty()) {
      Result<T> record = std::move(records.front());
      records.pop();
      return record;
    }

    if (error.isSome()) {
      return process::Failure(error.get().message);
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
  virtual void initialize() override
  {
    consume();
  }

  virtual void finalize() override
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
    if (read.get().empty()) {
      complete();
      return;
    }

    Try<std::deque<Try<T>>> decode = decoder.decode(read.get());

    if (decode.isError()) {
      fail("Decoder failure: " + decode.error());
      return;
    }

    foreach (const Try<T>& record, decode.get()) {
      if (!waiters.empty()) {
        waiters.front()->set(Result<T>(std::move(record)));
        waiters.pop();
      } else {
        records.push(std::move(record));
      }
    }

    consume();
  }

  ::recordio::Decoder<T> decoder;
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
