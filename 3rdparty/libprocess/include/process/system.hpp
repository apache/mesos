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

#ifndef __PROCESS_SYSTEM_HPP__
#define __PROCESS_SYSTEM_HPP__

#include <string>

#include <process/future.hpp>
#include <process/help.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/os.hpp>

namespace process {

// The System process provides HTTP endpoints for retrieving system metrics,
// such as CPU load and memory usage. This is started by default during the
// initialization of libprocess.
class System : public Process<System>
{
public:
  System()
    : ProcessBase("system"),
      load_1min(
          self().id + "/load_1min",
          defer(self(), &System::_load_1min)),
      load_5min(
          self().id + "/load_5min",
          defer(self(), &System::_load_5min)),
      load_15min(
          self().id + "/load_15min",
          defer(self(), &System::_load_15min)),
      cpus_total(
          self().id + "/cpus_total",
          defer(self(), &System::_cpus_total)),
      mem_total_bytes(
          self().id + "/mem_total_bytes",
          defer(self(), &System::_mem_total_bytes)),
      mem_free_bytes(
          self().id + "/mem_free_bytes",
          defer(self(), &System::_mem_free_bytes)) {}

  ~System() override {}

protected:
  void initialize() override
  {
    // TODO(dhamon): Check return values.
    metrics::add(load_1min);
    metrics::add(load_5min);
    metrics::add(load_15min);
    metrics::add(cpus_total);
    metrics::add(mem_total_bytes);
    metrics::add(mem_free_bytes);

    route("/stats.json", statsHelp(), &System::stats);
  }

  void finalize() override
  {
    metrics::remove(load_1min);
    metrics::remove(load_5min);
    metrics::remove(load_15min);
    metrics::remove(cpus_total);
    metrics::remove(mem_total_bytes);
    metrics::remove(mem_free_bytes);
  }

private:
  static std::string statsHelp()
  {
    return HELP(
      TLDR(
          "Shows local system metrics."),
      DESCRIPTION(
          ">        cpus_total          Total number of available CPUs",
          ">        load_1min           Average system load for last"
          " minute in uptime(1) style",
          ">        load_5min           Average system load for last"
          " 5 minutes in uptime(1) style",
          ">        load_15min          Average system load for last"
          " 15 minutes in uptime(1) style",
          ">        memory_total_bytes  Total system memory in bytes",
          ">        memory_free_bytes   Free system memory in bytes"));
  }

  // Gauge handlers.
  Future<double> _load_1min()
  {
    Try<os::Load> load = os::loadavg();
    if (load.isSome()) {
      return load->one;
    }
    return Failure("Failed to get loadavg: " + load.error());
  }


  Future<double> _load_5min()
  {
    Try<os::Load> load = os::loadavg();
    if (load.isSome()) {
      return load->five;
    }
    return Failure("Failed to get loadavg: " + load.error());
  }


  Future<double> _load_15min()
  {
    Try<os::Load> load = os::loadavg();
    if (load.isSome()) {
      return load->fifteen;
    }
    return Failure("Failed to get loadavg: " + load.error());
  }


  Future<double> _cpus_total()
  {
    Try<long> cpus = os::cpus();
    if (cpus.isSome()) {
      return cpus.get();
    }
    return Failure("Failed to get cpus: " + cpus.error());
  }


  Future<double> _mem_total_bytes()
  {
    Try<os::Memory> memory = os::memory();
    if (memory.isSome()) {
      return static_cast<double>(memory->total.bytes());
    }
    return Failure("Failed to get memory: " + memory.error());
  }


  Future<double> _mem_free_bytes()
  {
    Try<os::Memory> memory = os::memory();
    if (memory.isSome()) {
      return static_cast<double>(memory->free.bytes());
    }
    return Failure("Failed to get memory: " + memory.error());
  }

  // HTTP endpoints.
  Future<http::Response> stats(const http::Request& request)
  {
    JSON::Object object;
    Try<os::Load> load = os::loadavg();
    if (load.isSome()) {
      object.values["avg_load_1min"] = load->one;
      object.values["avg_load_5min"] = load->five;
      object.values["avg_load_15min"] = load->fifteen;
    }

    Try<long> cpus = os::cpus();
    if (cpus.isSome()) {
      object.values["cpus_total"] = cpus.get();
    }

    Try<os::Memory> memory = os::memory();
    if (memory.isSome()) {
      object.values["mem_total_bytes"] = memory->total.bytes();
      object.values["mem_free_bytes"] = memory->free.bytes();
    }

    return http::OK(object, request.url.query.get("jsonp"));
  }

  metrics::PullGauge load_1min;
  metrics::PullGauge load_5min;
  metrics::PullGauge load_15min;

  metrics::PullGauge cpus_total;

  metrics::PullGauge mem_total_bytes;
  metrics::PullGauge mem_free_bytes;
};

} // namespace process {

#endif // __PROCESS_SYSTEM_HPP__
