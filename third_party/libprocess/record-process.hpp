#ifndef RECORD_PROCESS_HPP
#define RECORD_PROCESS_HPP

#include <sstream>
#include <string>
#include <utility>

#include <process.hpp>

#include <boost/tuple/tuple.hpp>

#include <boost/variant/variant.hpp>

namespace process { namespace record {

struct serializer
{
  std::ostringstream& stream;

  serializer(std::ostringstream& s) : stream(s) {}

  void operator & (const ::boost::tuples::null_type &);
  void operator & (const int32_t &);
  void operator & (const int64_t &);
  void operator & (const size_t &);
  void operator & (const std::string &);
  void operator & (const PID &);
};

struct deserializer
{
  std::istringstream &stream;

  deserializer(std::istringstream &s) : stream(s) {}

  void operator & (int32_t &);
  void operator & (int64_t &);
  void operator & (size_t &);
  void operator & (std::string &);
  void operator & (PID &);
};
  
// Future named record macros:

// RECORD(REGISTER, (name of string,
//                   data of string,
//                   fid of FrameworkID,
//                   index of int));

// RECORD(REGISTER, ((string)(name),
//                   (string)(data),
//                   (FrameworkID)(fid),
//                   (int)(index)));

// RECORD(REGISTER, ((name)(string),
//                   (data)(string),
//                   (fid)(FrameworkID),
//                   (index)(int)));

template <MSGID ID> struct record;

#define IDENTITY(...) __VA_ARGS__

#define RECORD(ID, types)                          \
template <> struct record<ID>                      \
{                                                  \
   typedef ::boost::tuple<IDENTITY types> type;    \
   mutable type t;                                 \
   record(const type &_t) : t(_t) {}               \
}

template <MSGID ID>
struct size {
  static const int value = ::boost::tuples::length<typename record<ID>::type>::value;
};

template <bool b, int n, MSGID ID>
struct field_impl
{
  typedef typename ::boost::tuples::element<n, typename record<ID>::type>::type type;
};

template <int n, MSGID ID>
struct field_impl<false, n, ID>
{
  typedef ::boost::tuples::null_type type;
};

template <int n, MSGID ID>
struct field
{
  typedef typename field_impl<n < size<ID>::value, n, ID>::type type;
};

template <bool b, int n, MSGID ID>
struct __at
{
  static typename field<n, ID>::type impl(const record<ID> &r)
  {
    return ::boost::tuples::get<n>(r.t);
  }
};

template <int n, MSGID ID>
struct __at<false, n, ID>
{
  static ::boost::tuples::null_type impl(const record<ID> &r)
  {
    return ::boost::tuples::null_type();
  }
};

template <int n, MSGID ID>
typename field<n, ID>::type
at(const record<ID> &r)
{
  return __at<n < size<ID>::value, n, ID>::impl(r);
}


class RecordProcess : public Process
{
protected:
  template <MSGID ID>
  record<ID> pack()
  {
    return record<ID>(::boost::make_tuple());
  }

  template <MSGID ID>
  record<ID> pack(typename field<0, ID>::type t0)
  {
    return record<ID>(::boost::make_tuple(t0));
  }

  template <MSGID ID>
  record<ID> pack(typename field<0, ID>::type t0,
		  typename field<1, ID>::type t1)
  {
    return record<ID>(::boost::make_tuple(t0, t1));
  }

  template <MSGID ID>
  record<ID> pack(typename field<0, ID>::type t0,
		  typename field<1, ID>::type t1,
		  typename field<2, ID>::type t2)
  {
    return record<ID>(::boost::make_tuple(t0, t1, t2));
  }

  template <MSGID ID>
  record<ID> pack(typename field<0, ID>::type t0,
		  typename field<1, ID>::type t1,
		  typename field<2, ID>::type t2,
		  typename field<3, ID>::type t3)
  {
    return record<ID>(::boost::make_tuple(t0, t1, t2, t3));
  }

  template <MSGID ID>
  record<ID> pack(typename field<0, ID>::type t0,
		  typename field<1, ID>::type t1,
		  typename field<2, ID>::type t2,
		  typename field<3, ID>::type t3,
		  typename field<4, ID>::type t4)
  {
    return record<ID>(::boost::make_tuple(t0, t1, t2, t3, t4));
  }

  template <MSGID ID>
  record<ID> pack(typename field<0, ID>::type t0,
		  typename field<1, ID>::type t1,
		  typename field<2, ID>::type t2,
		  typename field<3, ID>::type t3,
		  typename field<4, ID>::type t4,
		  typename field<5, ID>::type t5)
  {
    return record<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5));
  }

  template <MSGID ID>
  record<ID> pack(typename field<0, ID>::type t0,
		  typename field<1, ID>::type t1,
		  typename field<2, ID>::type t2,
		  typename field<3, ID>::type t3,
		  typename field<4, ID>::type t4,
		  typename field<5, ID>::type t5,
		  typename field<6, ID>::type t6)
  {
    return record<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5, t6));
  }

  template <MSGID ID>
  record<ID> pack(typename field<0, ID>::type t0,
		  typename field<1, ID>::type t1,
		  typename field<2, ID>::type t2,
		  typename field<3, ID>::type t3,
		  typename field<4, ID>::type t4,
		  typename field<5, ID>::type t5,
		  typename field<6, ID>::type t6,
		  typename field<7, ID>::type t7)
  {
    return record<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5, t6, t7));
  }

  template <MSGID ID>
  record<ID> pack(typename field<0, ID>::type t0,
		  typename field<1, ID>::type t1,
		  typename field<2, ID>::type t2,
		  typename field<3, ID>::type t3,
		  typename field<4, ID>::type t4,
		  typename field<5, ID>::type t5,
		  typename field<6, ID>::type t6,
		  typename field<7, ID>::type t7,
		  typename field<8, ID>::type t8)
  {
    return record<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5, t6, t7, t8));
  }

  template <MSGID ID>
  record<ID> pack(typename field<0, ID>::type t0,
		  typename field<1, ID>::type t1,
		  typename field<2, ID>::type t2,
		  typename field<3, ID>::type t3,
		  typename field<4, ID>::type t4,
		  typename field<5, ID>::type t5,
		  typename field<6, ID>::type t6,
		  typename field<7, ID>::type t7,
		  typename field<8, ID>::type t8,
		  typename field<9, ID>::type t9)
  {
    return record<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9));
  }

  template <MSGID ID>
  void unpack(typename field<0, ID>::type &t0)
  {
    const char *data;
    size_t length;

    data = Process::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    deserializer d(is);

    d & t0;
  }

//   template <MSGID ID>
//   void
//   help(const ::boost::variant<typename field<0, ID>::type &,
// 	 ::boost::tuples::detail::swallow_assign &> &v0,
// 	 typename field<1, ID>::type &t1)
//   {
//     if (typename field<0, ID>::type *t0 = boost::get<typename field<0, ID>::type &>(&v0))
//     *pi *= 2;
//   else if ( std::string* pstr = boost::get<std::string>( &v ) )
//     *pstr += *pstr;
//     typedef typename record<ID>::type type;
//      ::boost::tie<typename field<0, ID>::type &,
//                         typename field<1, ID>::type &>(t0, t1) = type();
//   }

  template <MSGID ID>
  void unpack(typename field<0, ID>::type &t0,
	      typename field<1, ID>::type &t1)
  {
    const char *data;
    size_t length;

    data = Process::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    deserializer d(is);

    d & t0;
    d & t1;
  }


  template <MSGID ID>
  void unpack(typename field<0, ID>::type &t0,
	      typename field<1, ID>::type &t1,
	      typename field<2, ID>::type &t2)
  {
    const char *data;
    size_t length;

    data = Process::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    deserializer d(is);

    d & t0;
    d & t1;
    d & t2;
  }

  template <MSGID ID>
  void unpack(typename field<0, ID>::type &t0,
	      typename field<1, ID>::type &t1,
	      typename field<2, ID>::type &t2,
	      typename field<3, ID>::type &t3)
  {
    const char *data;
    size_t length;

    data = Process::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    deserializer d(is);

    d & t0;
    d & t1;
    d & t2;
    d & t3;
  }

  template <MSGID ID>
  void unpack(typename field<0, ID>::type &t0,
	      typename field<1, ID>::type &t1,
	      typename field<2, ID>::type &t2,
	      typename field<3, ID>::type &t3,
	      typename field<4, ID>::type &t4)
  {
    const char *data;
    size_t length;

    data = Process::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    deserializer d(is);

    d & t0;
    d & t1;
    d & t2;
    d & t3;
    d & t4;
  }

  template <MSGID ID>
  void unpack(typename field<0, ID>::type &t0,
	      typename field<1, ID>::type &t1,
	      typename field<2, ID>::type &t2,
	      typename field<3, ID>::type &t3,
	      typename field<4, ID>::type &t4,
	      typename field<5, ID>::type &t5)
  {
    const char *data;
    size_t length;

    data = Process::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    deserializer d(is);

    d & t0;
    d & t1;
    d & t2;
    d & t3;
    d & t4;
    d & t5;
  }

  template <MSGID ID>
  void unpack(typename field<0, ID>::type &t0,
	      typename field<1, ID>::type &t1,
	      typename field<2, ID>::type &t2,
	      typename field<3, ID>::type &t3,
	      typename field<4, ID>::type &t4,
	      typename field<5, ID>::type &t5,
	      typename field<6, ID>::type &t6)
  {
    const char *data;
    size_t length;

    data = Process::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    deserializer d(is);

    d & t0;
    d & t1;
    d & t2;
    d & t3;
    d & t4;
    d & t5;
    d & t6;
  }

  template <MSGID ID>
  void unpack(typename field<0, ID>::type &t0,
	      typename field<1, ID>::type &t1,
	      typename field<2, ID>::type &t2,
	      typename field<3, ID>::type &t3,
	      typename field<4, ID>::type &t4,
	      typename field<5, ID>::type &t5,
	      typename field<6, ID>::type &t6,
	      typename field<7, ID>::type &t7)
  {
    const char *data;
    size_t length;

    data = Process::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    deserializer d(is);

    d & t0;
    d & t1;
    d & t2;
    d & t3;
    d & t4;
    d & t5;
    d & t6;
    d & t7;
  }

  template <MSGID ID>
  void unpack(typename field<0, ID>::type &t0,
	      typename field<1, ID>::type &t1,
	      typename field<2, ID>::type &t2,
	      typename field<3, ID>::type &t3,
	      typename field<4, ID>::type &t4,
	      typename field<5, ID>::type &t5,
	      typename field<6, ID>::type &t6,
	      typename field<7, ID>::type &t7,
	      typename field<8, ID>::type &t8)
  {
    const char *data;
    size_t length;

    data = Process::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    deserializer d(is);

    d & t0;
    d & t1;
    d & t2;
    d & t3;
    d & t4;
    d & t5;
    d & t6;
    d & t7;
    d & t8;
  }

  template <MSGID ID>
  void unpack(typename field<0, ID>::type &t0,
	      typename field<1, ID>::type &t1,
	      typename field<2, ID>::type &t2,
	      typename field<3, ID>::type &t3,
	      typename field<4, ID>::type &t4,
	      typename field<5, ID>::type &t5,
	      typename field<6, ID>::type &t6,
	      typename field<7, ID>::type &t7,
	      typename field<8, ID>::type &t8,
	      typename field<9, ID>::type &t9)
  {
    const char *data;
    size_t length;

    data = Process::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    deserializer d(is);

    d & t0;
    d & t1;
    d & t2;
    d & t3;
    d & t4;
    d & t5;
    d & t6;
    d & t7;
    d & t8;
    d & t9;
  }

  template <MSGID ID>
  void send(const PID &to, const record<ID> &r)
  {
    std::ostringstream os;

    serializer s(os);

    if (size<ID>::value >= 1) {
      const typename field<0, ID>::type &temp = at<0>(r);
      s & temp;
    }

    if (size<ID>::value >= 2) {
      const typename field<1, ID>::type &temp = at<1>(r);
      s & temp;
    }
 
    if (size<ID>::value >= 3) {
      const typename field<2, ID>::type &temp = at<2>(r);
      s & temp;
    }

    if (size<ID>::value >= 4) {
      const typename field<3, ID>::type &temp = at<3>(r);
      s & temp;
    }

    if (size<ID>::value >= 5) {
      const typename field<4, ID>::type &temp = at<4>(r);
      s & temp;
    }

    if (size<ID>::value >= 6) {
      const typename field<5, ID>::type &temp = at<5>(r);
      s & temp;
    }

    if (size<ID>::value >= 7) {
      const typename field<6, ID>::type &temp = at<6>(r);
      s & temp;
    }

    if (size<ID>::value >= 8) {
      const typename field<7, ID>::type &temp = at<7>(r);
      s & temp;
    }

    if (size<ID>::value >= 9) {
      const typename field<8, ID>::type &temp = at<8>(r);
      s & temp;
    }

    if (size<ID>::value >= 10) {
      const typename field<9, ID>::type &temp = at<9>(r);
      s & temp;
    }

    std::string data = os.str();

    Process::send(to, ID, data.data(), data.size());
  }

};

}}

#endif /* RECORD_PROCESS_HPP */

