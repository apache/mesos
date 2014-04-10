#ifndef __PROCESS_METRICS_COUNTER_HPP__
#define __PROCESS_METRICS_COUNTER_HPP__

#include <string>

#include <process/metrics/metric.hpp>

namespace process {
namespace metrics {

// A Metric that represents an integer value that can be incremented and
// decremented.
class Counter : public Metric
{
public:
  explicit Counter(const std::string& name)
    : Metric(name),
      data(new Data()) {}

  virtual ~Counter() {}

  virtual Future<double> value() const
  {
    return static_cast<double>(data->v);
  }

  void reset()
  {
    __sync_fetch_and_and(&data->v, 0);
  }

  Counter& operator ++ ()
  {
    return *this += 1;
  }

  Counter operator ++ (int)
  {
    Counter c(*this);
    ++(*this);
    return c;
  }

  Counter& operator += (int64_t v)
  {
    __sync_fetch_and_add(&data->v, v);
    return *this;
  }

private:
  struct Data
  {
    explicit Data() : v(0) {}

    // TODO(dhamon): Update to std::atomic<int64_t> when C++11 lands.
    volatile int64_t v;
  };

  boost::shared_ptr<Data> data;
};

}  // namespace metrics {
}  // namespace process {

#endif  // __PROCESS_METRICS_COUNTER_HPP__
