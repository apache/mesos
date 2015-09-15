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

#ifndef __PROCESS_DISPATCHER_HPP__
#define __PROCESS_DISPATCHER_HPP__

#include <memory>
#include <utility>

#include <stout/try.hpp>

#include <process/dispatch.hpp>
#include <process/process.hpp>
#include <process/shared.hpp>

namespace mesos {

template <typename I>
class Dispatchable
{
public:
  virtual ~Dispatchable(){}

  template <typename Callable, typename ...Args>
    auto dispatch(Callable fn, Args&&... args)
    -> typename std::result_of<decltype(fn)(I*, Args...)>::type
    {
      ProcessBase* process = dynamic_cast<ProcessBase*>(getInterface());
      return process::dispatch<I, Callable, Args...>(
          process,
          fn,
          std::forward<Args>(args)...);
    }

protected:
  void setInterface(const process::Shared<I>& i)
  {
    interface = i;
  }

  I* getInterface()
  {
    return const_cast<I*>(interface.get());
  }

  process::Shared<I> interface;
};

template <typename I, typename P = I>
class ProcessDispatcher : public Dispatchable<I>
{
public:
  template<typename ...Args>
  static Try<process::Owned<Dispatchable<I>>> create(Args&&... args)
  {
    Try<process::Owned<P>> newProcess(P::create(std::forward<Args>(args)...));

    if (newProcess.isError()) {
      return Error(newProcess.error());
    }

    process::Shared<I> sharedInterface(newProcess.get().release());

    return process::Owned<Dispatchable<I>>(
        new ProcessDispatcher(sharedInterface));
  }

  static Try<process::Owned<ProcessDispatcher>> create(
      process::Shared<I> process)
  {
    return(process::Owned<ProcessDispatcher>(new ProcessDispatcher(process)));
  }

  ~ProcessDispatcher()
  {
    ProcessBase* process = dynamic_cast<ProcessBase*>(this->getInterface());

    terminate(process);
    process::wait(process);
  }

private:
  ProcessDispatcher(const process::Shared<I>& i)
    :Dispatchable<I>()
  {
    this->setInterface(i);

    ProcessBase* process = dynamic_cast<ProcessBase*>(const_cast<I*>(i.get()));
    if (!process)
    {
      assert(false);
    }

    spawn(process);
  }

  ProcessDispatcher(const ProcessDispatcher&) = delete;
};

} // namespace mesos {

#endif // __PROCESS_DISPATCHER_HPP__
