#include <stddef.h> // For size_t.

#include <process/timeseries.hpp>

#include <stout/duration.hpp>

namespace process {

// TODO(bmahler): Move these into timeseries.hpp header once we
// can require gcc >= 4.2.1.
const Duration TIME_SERIES_WINDOW = Weeks(2);
const size_t TIME_SERIES_CAPACITY = 1000;

}  // namespace process {
