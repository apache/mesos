#ifndef __PROCESS_METRICS_METRIC_HPP__
#define __PROCESS_METRICS_METRIC_HPP__

#include <string>

#include <boost/smart_ptr/shared_ptr.hpp>

#include <process/future.hpp>

namespace process {
namespace metrics {

// The base class for Metrics such as Counter and Gauge.
class Metric {
public:
  virtual ~Metric() {}

  virtual Future<double> value() const = 0;

  const std::string& name() const { return data->name; }

protected:
  // Only derived classes can construct.
  explicit Metric(const std::string& name)
    : data(new Data(name)) {}

private:
  struct Data {
    explicit Data(const std::string& _name) : name(_name) {}

    const std::string name;
  };

  boost::shared_ptr<Data> data;
};

}  // namespace metrics {
}  // namespace process {

#endif  // __PROCESS_METRICS_METRIC_HPP__
