#ifndef __PROCESS_METRICS_GAUGE_HPP__
#define __PROCESS_METRICS_GAUGE_HPP__

#include <string>

#include <process/async.hpp>
#include <process/defer.hpp>

#include <process/metrics/metric.hpp>

#include <stout/lambda.hpp>
#include <stout/memory.hpp>
#include <stout/none.hpp>

namespace process {
namespace metrics {

// A Metric that represents an instantaneous value evaluated when
// 'value' is called.
class Gauge : public Metric
{
public:
  // 'name' is the unique name for the instance of Gauge being constructed.
  // It will be the key exposed in the JSON endpoint.
  // 'f' is the function called asynchronously when the Metric value is
  // requested.
  Gauge(const std::string& name, const lambda::function<double(void)>& f)
    : Metric(name, None()),
      data(new Data(f)) {}

  // 'f' is the deferred object called when the Metric value is requested.
  Gauge(const std::string& name, const Deferred<Future<double>(void)>& f)
    : Metric(name, None()),
      data(new Data(f)) {}

  virtual ~Gauge() {}

  virtual Future<double> value() const { return data->f(); }

private:
  struct Data
  {
    // Wrap the function in an async call. The extra NULL parameter is required
    // as async takes a third parameter that defaults to NULL.
    explicit Data(const lambda::function<double (void)>& _f)
      : f(lambda::bind(&async<lambda::function<double (void)> >,
                       _f,
                       static_cast<void*>(NULL))) {}

    explicit Data(const Deferred<Future<double> (void)>& _f)
      : f(_f) {}

    const lambda::function<Future<double> (void)> f;
  };

  memory::shared_ptr<Data> data;
};

} // namespace metrics {
} // namespace process {

#endif // __PROCESS_METRICS_GAUGE_HPP__
