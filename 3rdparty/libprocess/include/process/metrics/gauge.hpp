#ifndef __PROCESS_METRICS_GAUGE_HPP__
#define __PROCESS_METRICS_GAUGE_HPP__

#include <string>

#include <process/defer.hpp>

#include <process/metrics/metric.hpp>

#include <stout/memory.hpp>

namespace process {
namespace metrics {

// A Metric that represents an instantaneous value evaluated when
// 'value' is called.
class Gauge : public Metric
{
public:
  // 'name' is the unique name for the instance of Gauge being constructed.
  // It will be the key exposed in the JSON endpoint.
  // 'f' is the deferred object called when the Metric value is requested.
  Gauge(const std::string& name,
        const Deferred<Future<double> (void)>& f)
    : Metric(name, None()),
      data(new Data(f)) {}

  virtual ~Gauge() {}

  virtual Future<double> value() const { return data->f(); }

private:
  struct Data
  {
    explicit Data(const Deferred<Future<double> (void)>& _f)
      : f(_f) {}

    const Deferred<Future<double> (void)> f;
  };

  memory::shared_ptr<Data> data;
};

} // namespace metrics {
} // namespace process {

#endif // __PROCESS_METRICS_GAUGE_HPP__
