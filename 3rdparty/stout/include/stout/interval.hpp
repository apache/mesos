// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// limitations under the License.

#ifndef __STOUT_INTERVAL_HPP__
#define __STOUT_INTERVAL_HPP__

#include <functional> // For std::less.
#include <iostream>

#include <boost/icl/interval.hpp>
#include <boost/icl/interval_set.hpp>


// Forward declarations.
template <typename T>
class Interval;


template <typename T>
class IntervalSet;


// Represents a bound (left or right) for an interval. A bound can
// either be open or closed.
template <typename T>
class Bound
{
public:
  // Creates an open bound.
  static Bound<T> open(const T& value)
  {
    return Bound<T>(OPEN, value);
  }

  // Creates an closed bound.
  static Bound<T> closed(const T& value)
  {
    return Bound<T>(CLOSED, value);
  }

  // Intervals can be created using the comma operator. For example:
  //   (2, 6):  (Bound<int>::open(2), Bound<int>::open(6))
  //   (3, 4]:  (Bound<int>::open(3), Bound<int>::closed(4))
  //   [0, 5):  (Bound<int>::closed(0), Bound<int>::open(5))
  //   [1, 2]:  (Bound<int>::closed(1), Bound<int>::closed(2))
  Interval<T> operator , (const Bound<T>& right) const;

private:
  enum Type
  {
    OPEN,
    CLOSED,
  };

  Bound(const Type _type, const T& _value)
    : type(_type), value(_value) {}

  const Type type;
  const T value;
};


// Represents an interval.
template <typename T>
class Interval
{
public:
  // We must provide a public default constructor as the boost
  // interval set expects that.
  Interval() {}

  // Returns the inclusive lower bound of this interval.
  T lower() const { return data.lower(); }

  // Returns the exclusive upper bound of this interval.
  T upper() const { return data.upper(); }

  // Checks if this interval intersects with another interval.
  bool intersects(const Interval<T>& interval) const;

  // Checks if this interval intersects with an interval set.
  bool intersects(const IntervalSet<T>& set) const;

  bool operator==(const Interval<T>& that) const
  {
    return data == that.data;
  }

  bool operator!=(const Interval<T>& that) const
  {
    return !(*this == that);
  }

private:
  friend class Bound<T>;

  template <typename X>
  friend std::ostream& operator<<(
      std::ostream& stream,
      const Interval<X>& interval);

  Interval(const boost::icl::right_open_interval<T, std::less>& _data)
    : data(_data) {}

  // We store the interval in right open format: [lower, upper).
  // Notice that we use composition here instead of inheritance to
  // prevent Interval from being passed to bare boost icl containers.
  boost::icl::right_open_interval<T, std::less> data;
};


template <typename T>
std::ostream& operator<<(std::ostream& stream, const Interval<T>& interval)
{
  return stream << interval.data;
}


template <typename T>
Interval<T> Bound<T>::operator , (const Bound<T>& right) const
{
  if (type == OPEN) {
    if (right.type == OPEN) {
      // For example: (1, 3).
      return boost::icl::static_interval<
          boost::icl::right_open_interval<T, std::less>, // Target.
          boost::icl::is_discrete<T>::value,
          boost::icl::interval_bounds::static_open,      // Input bounds.
          boost::icl::interval_bounds::static_right_open // Output bounds.
      >::construct(value, right.value);
    } else {
      // For example: (1, 3].
      return boost::icl::static_interval<
          boost::icl::right_open_interval<T, std::less>, // Target.
          boost::icl::is_discrete<T>::value,
          boost::icl::interval_bounds::static_left_open, // Input bounds.
          boost::icl::interval_bounds::static_right_open // Output bounds.
      >::construct(value, right.value);
    }
  } else {
    if (right.type == OPEN) {
      // For example: [1, 3).
      return boost::icl::static_interval<
          boost::icl::right_open_interval<T, std::less>,  // Target.
          boost::icl::is_discrete<T>::value,
          boost::icl::interval_bounds::static_right_open, // Input bounds.
          boost::icl::interval_bounds::static_right_open  // Output bounds.
      >::construct(value, right.value);
    } else {
      // For example: [1, 3].
      return boost::icl::static_interval<
          boost::icl::right_open_interval<T, std::less>,  // Target.
          boost::icl::is_discrete<T>::value,
          boost::icl::interval_bounds::static_closed,     // Input bounds.
          boost::icl::interval_bounds::static_right_open  // Output bounds.
      >::construct(value, right.value);
    }
  }
}


// Modeled after boost interval_set. Provides a compact representation
// of a set by merging adjacent elements into intervals.
template <typename T>
class IntervalSet : public boost::icl::interval_set<T, std::less, Interval<T>>
{
public:
  IntervalSet() {}

  explicit IntervalSet(const T& value)
  {
    Base::add(value);
  }

  explicit IntervalSet(const Interval<T>& interval)
  {
    Base::add(interval);
  }

  IntervalSet(const Bound<T>& lower, const Bound<T>& upper)
  {
    Base::add((lower, upper));
  }

  // Checks if an element is in this set.
  bool contains(const T& value) const
  {
    return boost::icl::contains(static_cast<const Base&>(*this), value);
  }

  // Checks if an interval is in this set.
  bool contains(const Interval<T>& interval) const
  {
    // TODO(jieyu): Boost has an issue regarding unqualified lookup in
    // template (http://clang.llvm.org/compatibility.html#dep_lookup),
    // and gcc-4.8 complains about it. We use a workaround here by
    // delegating this call to the IntervalSet version below.
    return contains(IntervalSet<T>(interval));
  }

  // Checks if an interval set is a subset of this set.
  bool contains(const IntervalSet<T>& set) const
  {
    return boost::icl::contains(
        static_cast<const Base&>(*this),
        static_cast<const Base&>(set));
  }

  // Checks if this set intersects with an interval.
  bool intersects(const Interval<T>& interval) const
  {
    return boost::icl::intersects(static_cast<const Base&>(*this), interval);
  }

  // Checks if this set intersects with another interval set.
  bool intersects(const IntervalSet<T>& set) const
  {
    return boost::icl::intersects(
        static_cast<const Base&>(*this),
        static_cast<const Base&>(set));
  }

  // Returns the number of intervals in this set.
  size_t intervalCount() const
  {
    return boost::icl::interval_count(static_cast<const Base&>(*this));
  }

  // Overloaded operators.
  bool operator==(const IntervalSet<T>& that) const
  {
    return static_cast<const Base&>(*this) == static_cast<const Base&>(that);
  }

  bool operator!=(const IntervalSet<T>& that) const
  {
    return !(*this == that);
  }

  IntervalSet<T>& operator+=(const T& value)
  {
    static_cast<Base&>(*this) += value;
    return *this;
  }

  IntervalSet<T>& operator+=(const Interval<T>& interval)
  {
    static_cast<Base&>(*this) += interval;
    return *this;
  }

  IntervalSet<T>& operator+=(const IntervalSet<T>& set)
  {
    static_cast<Base&>(*this) += static_cast<const Base&>(set);
    return *this;
  }

  IntervalSet<T>& operator-=(const T& value)
  {
    static_cast<Base&>(*this) -= value;
    return *this;
  }

  IntervalSet<T>& operator-=(const Interval<T>& interval)
  {
    static_cast<Base&>(*this) -= interval;
    return *this;
  }

  IntervalSet<T>& operator-=(const IntervalSet<T>& set)
  {
    static_cast<Base&>(*this) -= static_cast<const Base&>(set);
    return *this;
  }

  IntervalSet<T>& operator&=(const T& value)
  {
    static_cast<Base&>(*this) &= value;
    return *this;
  }

  IntervalSet<T>& operator&=(const Interval<T>& interval)
  {
    static_cast<Base&>(*this) &= interval;
    return *this;
  }

  IntervalSet<T>& operator&=(const IntervalSet<T>& set)
  {
    static_cast<Base&>(*this) &= static_cast<const Base&>(set);
    return *this;
  }

private:
  template <typename X>
  friend std::ostream& operator<<(
      std::ostream& stream,
      const IntervalSet<X>& set);

  // We use typedef here to make the code less verbose.
  typedef boost::icl::interval_set<T, std::less, Interval<T>> Base;
};


template <typename T>
std::ostream& operator<<(std::ostream& stream, const IntervalSet<T>& set)
{
  return stream << static_cast<const typename IntervalSet<T>::Base&>(set);
}


template <typename T>
bool Interval<T>::intersects(const Interval<T>& interval) const
{
  return IntervalSet<T>(*this).intersects(interval);
}


template <typename T>
bool Interval<T>::intersects(const IntervalSet<T>& set) const
{
  return set.intersects(*this);
}


template <typename T, typename X>
IntervalSet<T> operator+(const IntervalSet<T>& set, const X& x)
{
  IntervalSet<T> result(set);
  result += x;
  return result;
}


template <typename T, typename X>
IntervalSet<T> operator-(const IntervalSet<T>& set, const X& x)
{
  IntervalSet<T> result(set);
  result -= x;
  return result;
}


// Defines type traits for the custom Interval above. These type
// traits are required by the boost interval set.
namespace boost {
namespace icl {

template <typename T>
struct interval_traits<Interval<T>>
{
  typedef interval_traits type;
  typedef T domain_type;
  typedef std::less<T> domain_compare;

  static Interval<T> construct(const T& lower, const T& upper)
  {
    return (Bound<T>::closed(lower), Bound<T>::open(upper));
  }

  static T lower(const Interval<T>& interval)
  {
    return interval.lower();
  }

  static T upper(const Interval<T>& interval)
  {
    return interval.upper();
  }
};


template <typename T>
struct interval_bound_type<Interval<T>>
  : public interval_bound_type<right_open_interval<T, std::less>>
{
  typedef interval_bound_type type;
};

} // namespace icl {
} // namespace boost {

#endif // __STOUT_INTERVAL_HPP__
