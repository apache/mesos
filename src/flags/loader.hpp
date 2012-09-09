#ifndef __FLAGS_LOADER_HPP__
#define __FLAGS_LOADER_HPP__

#include <string>

#include <tr1/functional>

#include <stout/option.hpp>
#include <stout/try.hpp>

namespace flags {

// Forward declaration.
class FlagsBase;

template <typename T>
struct Loader
{
  static void load(T* flag,
                   const std::tr1::function<Try<T>(const std::string&)>& parse,
                   const std::string& name,
                   const std::string& value)
  {
    Try<T> t = parse(value);
    if (t.isSome()) {
      *flag = t.get();
    } else {
      std::cerr << "Failed to load value '" << value
                << "' for flag '" << name
                << "': " << t.error() << std::endl;
      abort();
    }
  }
};


template <typename T>
struct OptionLoader
{
  static void load(Option<T>* flag,
                   const std::tr1::function<Try<T>(const std::string&)>& parse,
                   const std::string& name,
                   const std::string& value)
  {
    Try<T> t = parse(value);
    if (t.isSome()) {
      *flag = Option<T>::some(t.get());
    } else {
      std::cerr << "Failed to load value '" << value
                << "' for flag '" << name
                << "': " << t.error() << std::endl;
      abort();
    }
  }
};


template <typename F, typename T>
struct MemberLoader
{
  static void load(FlagsBase* base,
                   T F::*flag,
                   const std::tr1::function<Try<T>(const std::string&)>& parse,
                   const std::string& name,
                   const std::string& value)
  {
    F* f = dynamic_cast<F*>(base);
    if (f != NULL) {
      Try<T> t = parse(value);
      if (t.isSome()) {
        f->*flag = t.get();
      } else {
        std::cerr << "Failed to load value '" << value
                  << "' for flag '" << name
                  << "': " << t.error() << std::endl;
        abort();
      }
    }
  }
};


template <typename F, typename T>
struct OptionMemberLoader
{
  static void load(FlagsBase* base,
                   Option<T> F::*flag,
                   const std::tr1::function<Try<T>(const std::string&)>& parse,
                   const std::string& name,
                   const std::string& value)
  {
    F* f = dynamic_cast<F*>(base);
    if (f != NULL) {
      Try<T> t = parse(value);
      if (t.isSome()) {
        f->*flag = Option<T>::some(t.get());
      } else {
        std::cerr << "Failed to load value '" << value
                  << "' for flag '" << name
                  << "': " << t.error() << std::endl;
        abort();
      }
    }
  }
};

} // namespace flags {

#endif // __FLAGS_LOADER_HPP__
