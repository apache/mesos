#ifndef __PROCESS_METRICS_COUNTER_HPP__
#define __PROCESS_METRICS_COUNTER_HPP__

#include <string>

#include <process/metrics/metric.hpp>

#include <stout/memory.hpp>

namespace process {
namespace metrics {

// A Metric that represents an integer value that can be incremented and
// decremented.
class Counter : public Metric
{
public:
  // 'name' is the unique name for the instance of Counter being constructed.
  // This is what will be used as the key in the JSON endpoint.
  // 'window' is the amount of history to keep for this Metric.
  Counter(const std::string& name, const Option<Duration>& window = None())
    : Metric(name, window),
      data(new Data())
  {
    push(data->v);
  }

  virtual ~Counter() {}

  virtual Future<double> value() const
  {
    return static_cast<double>(data->v);
  }

  void reset()
  {
    push(__sync_and_and_fetch(&data->v, 0));
  }

  Counter& operator ++ () // NOLINT(whitespace/operators)
  {
    return *this += 1;
  }

  Counter operator ++ (int) // NOLINT(whitespace/operators)
  {
    Counter c(*this);
    ++(*this);
    return c;
  }

  Counter& operator += (int64_t v)
  {
    push(__sync_add_and_fetch(&data->v, v));
    return *this;
  }

private:
  struct Data
  {
    explicit Data() : v(0) {}

    // TODO(dhamon): Update to std::atomic<int64_t> when C++11 lands.
    volatile int64_t v;
  };

  memory::shared_ptr<Data> data;
};

} // namespace metrics {
} // namespace process {

#endif // __PROCESS_METRICS_COUNTER_HPP__
