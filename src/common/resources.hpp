#ifndef __RESOURCES_HPP__
#define __RESOURCES_HPP__

#include <iterator>
#include <string>

#include <mesos.hpp>


// Resources come in three types: scalar, ranges, and sets. These are
// represented using protocol buffers. To make manipulation of
// resources easier within the Mesos core we provide generic
// overloaded opertors (see below) as well as a general Resources
// class that encapsulates a collection of protocol buffer Resource
// objects. The Resources class also provides a few static routines to
// allow parsing resources (e.g., from the command line), as well as
// determining whether or not a Resource object is valid or
// allocatable. In particular, a scalar is allocatable if it's value
// is greater than zero, a ranges is allocatable if there is at least
// one valid range in it, and a set is allocatable if it has at least
// one item. One can get only the allocatable resources by calling the
// allocatable routine on a resources object. Note that many of these
// operations have not been optimized but instead just written for
// correct semantics.


// Note! A resource is described by a tuple (name, type). Doing
// "arithmetic" operations (those defined below) on two resources of
// the same name but different type doesn't make sense, so it's
// semantics are as though the second operand was actually just and
// empty resource (as though you didn't do the operation at all). In
// addition, doing operations on two resources of the same type but
// different names is a no-op.

// Parsing resources can be done via the Resources::parse
// routines. The syntax currently requires that resources are
// separated by semicolons, which means on the command line the option
// needs to be quoted (whitespace is ignored). A scalar is just a
// number, a range is described like "[2-10, 34-56]", and a set like
// "{a, b, c, d}".


namespace mesos {

bool operator == (const Resource::Scalar& left, const Resource::Scalar& right);
bool operator <= (const Resource::Scalar& left, const Resource::Scalar& right);
Resource::Scalar operator + (const Resource::Scalar& left, const Resource::Scalar& right);
Resource::Scalar operator - (const Resource::Scalar& left, const Resource::Scalar& right);
Resource::Scalar& operator += (Resource::Scalar& left, const Resource::Scalar& right);
Resource::Scalar& operator -= (Resource::Scalar& left, const Resource::Scalar& right);


bool operator == (const Resource::Ranges& left, const Resource::Ranges& right);
bool operator <= (const Resource::Ranges& left, const Resource::Ranges& right);
Resource::Ranges operator + (const Resource::Ranges& left, const Resource::Ranges& right);
Resource::Ranges operator - (const Resource::Ranges& left, const Resource::Ranges& right);
Resource::Ranges& operator += (Resource::Ranges& left, const Resource::Ranges& right);
Resource::Ranges& operator -= (Resource::Ranges& left, const Resource::Ranges& right);


bool operator == (const Resource::Set& left, const Resource::Set& right);
bool operator <= (const Resource::Set& left, const Resource::Set& right);
Resource::Set operator + (const Resource::Set& left, const Resource::Set& right);
Resource::Set operator - (const Resource::Set& left, const Resource::Set& right);
Resource::Set& operator += (Resource::Set& left, const Resource::Set& right);
Resource::Set& operator -= (Resource::Set& left, const Resource::Set& right);


bool operator == (const Resource& left, const Resource& right);
bool operator <= (const Resource& left, const Resource& right);
Resource operator + (const Resource& left, const Resource& right);
Resource operator - (const Resource& left, const Resource& right);
Resource& operator += (Resource& left, const Resource& right);
Resource& operator -= (Resource& left, const Resource& right);

std::ostream& operator << (std::ostream& stream, const Resource& resource);


namespace internal {

class Resources
{
public:
  Resources() {}

  Resources(const google::protobuf::RepeatedPtrField<Resource>& _resources)
  {
    resources.MergeFrom(_resources);
  }

  Resources(const Resources& that)
  {
    resources.MergeFrom(that.resources);
  }

  Resources& operator = (const Resources& that)
  {
    if (this != &that) {
      resources.Clear();
      resources.MergeFrom(that.resources);
    }

    return *this;
  }


  // Returns a Resources object with only the allocatable resources.
  Resources allocatable() const
  {
    Resources result;

    foreach (const Resource& resource, resources) {
      if (isAllocatable(resource)) {
        result.resources.Add()->MergeFrom(resource);
      }
    }

    return result;
  }


  size_t size() const
  {
    return resources.size();
  }


  // Using this operator makes it easy to copy a resources object into
  // a protocol buffer field.
  operator const google::protobuf::RepeatedPtrField<Resource>& () const
  {
    return resources;
  }


  bool operator == (const Resources& that) const
  {
    foreach (const Resource& resource, resources) {
      if (!that.contains(resource.name(), resource.type())) {
        return false;
      } else {
        if (!(resource == that.get(resource.name(), Resource()))) {
          return false;
        }
      }
    }

    return true;
  }


  bool operator <= (const Resources& that) const
  {
    foreach (const Resource& resource, resources) {
      if (!that.contains(resource.name(), resource.type())) {
        return false;
      } else {
        if (!(resource <= that.get(resource.name(), Resource()))) {
          return false;
        }
      }
    }

    return true;
  }


  Resources operator + (const Resources& that) const
  {
    Resources result(*this);

    foreach (const Resource& resource, that.resources) {
      result += resource;
    }

    return result;
  }

  
  Resources operator - (const Resources& that) const
  {
    Resources result(*this);

    foreach (const Resource& resource, that.resources) {
      result -= resource;
    }

    return result;
  }

  
  Resources& operator += (const Resources& that)
  {
    foreach (const Resource& resource, that.resources) {
      *this += resource;
    }

    return *this;
  }


  Resources& operator -= (const Resources& that)
  {
    foreach (const Resource& resource, that.resources) {
      *this -= resource;
    }

    return *this;
  }


  Resources operator + (const Resource& that) const
  {
    Resources result;

    bool added = false;

    foreach (const Resource& resource, resources) {
      if (resource.name() == that.name() && resource.type() == that.type()) {
        result.resources.Add()->MergeFrom(resource + that);
        added = true;
      } else {
        result.resources.Add()->MergeFrom(resource);
      }
    }

    if (!added) {
      result.resources.Add()->MergeFrom(that);
    }

    return result;
  }


  Resources operator - (const Resource& that) const
  {
    Resources result;

    foreach (const Resource& resource, resources) {
      if (resource.name() == that.name() && resource.type() == that.type()) {
        result.resources.Add()->MergeFrom(resource - that);
      } else {
        result.resources.Add()->MergeFrom(resource);
      }
    }

    return result;
  }

  
  Resources& operator += (const Resource& that)
  {
    *this = *this + that;
    return *this;
  }


  Resources& operator -= (const Resource& that)
  {
    *this = *this - that;
    return *this;
  }


  bool contains(const std::string& name, const Resource::Type& type) const
  {
    foreach (const Resource& resource, resources) {
      if (name == resource.name() && type == resource.type()) {
        return true;
      }
    }

    return false;
  }


  Resource get(const std::string& name, const Resource& resource) const
  {
    foreach (const Resource& resource, resources) {
      if (resource.name() == name) {
        return resource;
      }
    }

    return resource;
  }


  Resource::Scalar getScalar(const std::string& name, const Resource::Scalar& scalar) const
  {
    foreach (const Resource& resource, resources) {
      if (resource.name() == name && resource.type() == Resource::SCALAR) {
        return resource.scalar();
      }
    }

    return scalar;
  }


  Resource::Ranges getRanges(const std::string& name, const Resource::Ranges& ranges) const
  {
    foreach (const Resource& resource, resources) {
      if (resource.name() == name && resource.type() == Resource::RANGES) {
        return resource.ranges();
      }
    }

    return ranges;
  }


  Resource::Set getSet(const std::string& name, const Resource::Set& set) const
  {
    foreach (const Resource& resource, resources) {
      if (resource.name() == name && resource.type() == Resource::SET) {
        return resource.set();
      }
    }

    return set;
  }


  typedef google::protobuf::RepeatedPtrField<Resource>::iterator
  iterator;


  typedef google::protobuf::RepeatedPtrField<Resource>::const_iterator
  const_iterator;


  iterator begin() { return resources.begin(); }
  iterator end() { return resources.end(); }


  const_iterator begin() const { return resources.begin(); }
  const_iterator end() const { return resources.end(); }


  static Resource parse(const std::string& name, const std::string& value);
  static Resources parse(const std::string& s);


  static bool isValid(const Resource& resource)
  {
    if (!resource.has_name() ||
        resource.name() == "" ||
        !resource.has_type() ||
        !Resource::Type_IsValid(resource.type())) {
      return false;
    }

    if (resource.type() == Resource::SCALAR) {
      return resource.has_scalar();
    } else if (resource.type() == Resource::RANGES) {
      return resource.has_ranges();
    } else if (resource.type() == Resource::SET) {
      return resource.has_ranges();
    }

    return false;
  }


  static bool isAllocatable(const Resource& resource)
  {
    if (isValid(resource)) {
      if (resource.type() == Resource::SCALAR) {
        if (resource.scalar().value() <= 0) {
          return false;
        }
      } else if (resource.type() == Resource::RANGES) {
        if (resource.ranges().range_size() == 0) {
          return false;
        } else {
          for (int i = 0; i < resource.ranges().range_size(); i++) {
            const Resource::Range& range = resource.ranges().range(i);

            // Ensure the range make sense (isn't inverted).
            if (range.begin() > range.end()) {
              return false;
            }

            // Ensure ranges don't overlap (but not necessarily coalesced).
            for (int j = j + 1; j < resource.ranges().range_size(); j++) {
              if (range.begin() <= resource.ranges().range(j).begin() &&
                  resource.ranges().range(j).begin() <= range.end()) {
                return false;
              }
            }
          }
        }
      } else if (resource.type() == Resource::SET) {
        if (resource.set().item_size() == 0) {
          return false;
        } else {
          for (int i = 0; i < resource.set().item_size(); i++) {
            const std::string& item = resource.set().item(i);
            
            // Ensure no duplicates.
            for (int j = i + 1; j < resource.set().item_size(); j++) {
              if (item == resource.set().item(j)) {
                return false;
              }
            }
          }
        }
      }

      return true;
    }

    return false;
  }

private:
  google::protobuf::RepeatedPtrField<Resource> resources;
};


inline
std::ostream& operator << (std::ostream& stream, const Resources& resources)
{
  mesos::internal::Resources::const_iterator it = resources.begin();

  while (it != resources.end()) {
    stream << *it;
    if (++it != resources.end()) {
      stream << "; ";
    }
  }

  return stream;
}

}} // namespace mesos { namespace internal {

// namespace boost {

// template <>
// struct range_iterator<mesos::internal::Resources>
// {
//   typedef mesos::internal::Resources::iterator type;
// };

// template <>
// struct range_const_iterator<mesos::internal::Resources>
// {
//   typedef mesos::internal::Resources::const_iterator type;
// };

// } // namespace boost {


#endif // __RESOURCES_HPP__
