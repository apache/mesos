#ifndef __FLAGS_LOADER_HPP__
#define __FLAGS_LOADER_HPP__

#include <sstream> // For istringstream.
#include <string>

#include <tr1/functional>

#include "common/option.hpp"

namespace flags {

// Forward declaration.
class FlagsBase;

template <typename T>
struct Loader
{
  static void load(const std::string& name,
                   T* t,
                   const std::string& value)
  {
    std::istringstream in(value);
    in >> *t;
    if (!in.good() && !in.eof()) {
      std::cerr << "Failed to load value '" << value
                << "' for flag '" << name
                << "'" << std::endl;
      abort();
    }
  }
};


template <>
struct Loader<std::string>
{
  static void load(const std::string& name,
                   std::string* s,
                   const std::string& value)
  {
    *s = value;
  }
};


template <>
struct Loader<bool>
{
  static void load(const std::string& name,
                   bool* b,
                   const std::string& value)
  {
    if (value == "true" || value == "1") {
      *b = true;
    } else if (value == "false" || value == "0") {
      *b = false;
    } else {
      std::cerr << "Failed to load value '" << value
                << "' for flag '" << name
                << "'" << std::endl;
      abort();
    }
  }
};


template <typename T>
struct OptionLoader
{
  static void load(const std::string& name,
                   Option<T>* option,
                   const std::string& value)
  {
    T t;
    std::istringstream in(value);
    in >> t;
    if (!in.good() && !in.eof()) {
      std::cerr << "Failed to load value '" << value
                << "' for flag '" << name
                << "'" << std::endl;
      abort();
    }
    *option = Option<T>::some(t);
  }
};


template <>
struct OptionLoader<std::string>
{
  static void load(const std::string& name,
                   Option<std::string>* option,
                   const std::string& value)
  {
    *option = Option<std::string>(value);
  }
};


template <>
struct OptionLoader<bool>
{
  static void load(const std::string& name,
                   Option<bool>* option,
                   const std::string& value)
  {
    if (value == "true" || value == "1") {
      *option = Option<bool>::some(true);
    } else if (value == "false" || value == "0") {
      *option = Option<bool>::some(false);
    } else {
      std::cerr << "Failed to load value '" << value
                << "' for flag '" << name
                << "'" << std::endl;
      abort();
    }
  }
};


template <typename T, typename M>
struct MemberLoader
{
  static void load(const std::string& name,
                   M T::*m,
                   FlagsBase* base,
                   const std::string& value)
  {
    T* t = dynamic_cast<T*>(base);
    if (t != NULL) {
      std::istringstream in(value);
      in >> t->*m;
      if (!in.good() && !in.eof()) {
        std::cerr << "Failed to load value '" << value
                  << "' for flag '" << name
                  << "'" << std::endl;
        abort();
      }
    }
  }
};


template <typename T>
struct MemberLoader<T, std::string>
{
  static void load(const std::string& name,
                   std::string T::*s,
                   FlagsBase* base,
                   const std::string& value)
  {
    T* t = dynamic_cast<T*>(base);
    if (t != NULL) {
      t->*s = value;
    }
  }
};


template <typename T>
struct MemberLoader<T, bool>
{
  static void load(const std::string& name,
                   bool T::*b,
                   FlagsBase* base,
                   const std::string& value)
  {
    T* t = dynamic_cast<T*>(base);
    if (t != NULL) {
      if (value == "true" || value == "1") {
        t->*b = true;
      } else if (value == "false" || value == "0") {
        t->*b = false;
      } else {
        std::cerr << "Failed to load value '" << value
                  << "' for flag '" << name
                  << "'" << std::endl;
        abort();
      }
    }
  }
};


template <typename T, typename M>
struct OptionMemberLoader
{
  static void load(const std::string& name,
                   Option<M> T::*option,
                   FlagsBase* base,
                   const std::string& value)
  {
    T* t = dynamic_cast<T*>(base);
    if (t != NULL) {
      M m;
      std::istringstream in(value);
      in >> m;
      if (!in.good() && !in.eof()) {
        std::cerr << "Failed to load value '" << value
                  << "' for flag '" << name
                  << "'" << std::endl;
        abort();
      }
      t->*option = Option<T>::some(m);
    }
  }
};


template <typename T>
struct OptionMemberLoader<T, std::string>
{
  static void load(const std::string& name,
                   Option<std::string> T::*option,
                   FlagsBase* base,
                   const std::string& value)
  {
    T* t = dynamic_cast<T*>(base);
    if (t != NULL) {
      t->*option = Option<std::string>(value);
    }
  }
};


template <typename T>
struct OptionMemberLoader<T, bool>
{
  static void load(const std::string& name,
                   Option<bool> T::*option,
                   FlagsBase* base,
                   const std::string& value)
  {
    T* t = dynamic_cast<T*>(base);
    if (t != NULL) {
      if (value == "true" || value == "1") {
        t->*option = Option<bool>::some(true);
      } else if (value == "false" || value == "0") {
        t->*option = Option<bool>::some(false);
      } else {
        std::cerr << "Failed to load value '" << value
                  << "' for flag '" << name
                  << "'" << std::endl;
        abort();
      }
    }
  }
};

} // namespace flags {

#endif // __FLAGS_LOADER_HPP__
