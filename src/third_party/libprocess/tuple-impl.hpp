template <MSGID ID> struct tuple;

template <MSGID ID>
struct size {
  static const int value = ::boost::tuples::length<typename tuple<ID>::type>::value;
};

template <bool b, int n, MSGID ID>
struct field_impl
{
  typedef typename ::boost::tuples::element<n, typename tuple<ID>::type>::type type;
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
  static typename field<n, ID>::type impl(const tuple<ID> &r)
  {
    return ::boost::tuples::get<n>(r.t);
  }
};

template <int n, MSGID ID>
struct __at<false, n, ID>
{
  static ::boost::tuples::null_type impl(const tuple<ID> &r)
  {
    return ::boost::tuples::null_type();
  }
};

template <int n, MSGID ID>
typename field<n, ID>::type
at(const tuple<ID> &r)
{
  return __at<n < size<ID>::value, n, ID>::impl(r);
}

template <typename P>
class Tuple : public P
{
public:
  template <MSGID ID>
  static tuple<ID> pack()
  {
    return tuple<ID>(::boost::make_tuple());
  }

  template <MSGID ID>
  static tuple<ID> pack(typename field<0, ID>::type t0)
  {
    return tuple<ID>(::boost::make_tuple(t0));
  }

  template <MSGID ID>
  static tuple<ID> pack(typename field<0, ID>::type t0,
			typename field<1, ID>::type t1)
  {
    return tuple<ID>(::boost::make_tuple(t0, t1));
  }

  template <MSGID ID>
  static tuple<ID> pack(typename field<0, ID>::type t0,
			typename field<1, ID>::type t1,
			typename field<2, ID>::type t2)
  {
    return tuple<ID>(::boost::make_tuple(t0, t1, t2));
  }

  template <MSGID ID>
  static tuple<ID> pack(typename field<0, ID>::type t0,
			typename field<1, ID>::type t1,
			typename field<2, ID>::type t2,
			typename field<3, ID>::type t3)
  {
    return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3));
  }

  template <MSGID ID>
  static tuple<ID> pack(typename field<0, ID>::type t0,
			typename field<1, ID>::type t1,
			typename field<2, ID>::type t2,
			typename field<3, ID>::type t3,
			typename field<4, ID>::type t4)
  {
    return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3, t4));
  }

  template <MSGID ID>
  static tuple<ID> pack(typename field<0, ID>::type t0,
			typename field<1, ID>::type t1,
			typename field<2, ID>::type t2,
			typename field<3, ID>::type t3,
			typename field<4, ID>::type t4,
			typename field<5, ID>::type t5)
  {
    return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5));
  }

  template <MSGID ID>
  static tuple<ID> pack(typename field<0, ID>::type t0,
			typename field<1, ID>::type t1,
			typename field<2, ID>::type t2,
			typename field<3, ID>::type t3,
			typename field<4, ID>::type t4,
			typename field<5, ID>::type t5,
			typename field<6, ID>::type t6)
  {
    return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5, t6));
  }

  template <MSGID ID>
  static tuple<ID> pack(typename field<0, ID>::type t0,
			typename field<1, ID>::type t1,
			typename field<2, ID>::type t2,
			typename field<3, ID>::type t3,
			typename field<4, ID>::type t4,
			typename field<5, ID>::type t5,
			typename field<6, ID>::type t6,
			typename field<7, ID>::type t7)
  {
    return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5, t6, t7));
  }

  template <MSGID ID>
  static tuple<ID> pack(typename field<0, ID>::type t0,
			typename field<1, ID>::type t1,
			typename field<2, ID>::type t2,
			typename field<3, ID>::type t3,
			typename field<4, ID>::type t4,
			typename field<5, ID>::type t5,
			typename field<6, ID>::type t6,
			typename field<7, ID>::type t7,
			typename field<8, ID>::type t8)
  {
    return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5, t6, t7, t8));
  }

  template <MSGID ID>
  static tuple<ID> pack(typename field<0, ID>::type t0,
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
    return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9));
  }

protected:

  template <MSGID ID>
  void unpack(typename field<0, ID>::type &t0)
  {
    const char *data;
    size_t length;

    data = P::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    process::serialization::deserializer d(is);

    d & t0;
  }

  template <MSGID ID>
  void unpack(typename field<0, ID>::type &t0,
	      typename field<1, ID>::type &t1)
  {
    const char *data;
    size_t length;
    
    data = P::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    process::serialization::deserializer d(is);

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

    data = P::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    process::serialization::deserializer d(is);

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

    data = P::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    process::serialization::deserializer d(is);

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

    data = P::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    process::serialization::deserializer d(is);

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

    data = P::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    process::serialization::deserializer d(is);

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

    data = P::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    process::serialization::deserializer d(is);

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

    data = P::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    process::serialization::deserializer d(is);

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

    data = P::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    process::serialization::deserializer d(is);

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

    data = P::body(&length);
    std::string s(data, length);
    std::istringstream is(s);
    process::serialization::deserializer d(is);

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
  void send(const PID &to, const tuple<ID> &r)
  {
    std::ostringstream os;

    process::serialization::serializer s(os);

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

    P::send(to, ID, data.data(), data.size());
  }

  template <MSGID ID>
  void send(const PID &to)
  {
    send(to, pack<ID>());
  }

  template <MSGID ID>
  void send(const PID &to, typename field<0, ID>::type t0)
  {
    send(to, pack<ID>(t0));
  }

  template <MSGID ID>
  void send(const PID &to,
	    typename field<0, ID>::type t0,
	    typename field<1, ID>::type t1)
  {
    send(to, pack<ID>(t0, t1));
  }

  template <MSGID ID>
  void send(const PID &to,
	    typename field<0, ID>::type t0,
	    typename field<1, ID>::type t1,
	    typename field<2, ID>::type t2)
  {
    send(to, pack<ID>(t0, t1, t2));
  }

  template <MSGID ID>
  void send(const PID &to,
	    typename field<0, ID>::type t0,
	    typename field<1, ID>::type t1,
	    typename field<2, ID>::type t2,
	    typename field<3, ID>::type t3)
  {
    send(to, pack<ID>(t0, t1, t2, t3));
  }

  template <MSGID ID>
  void send(const PID &to,
	    typename field<0, ID>::type t0,
	    typename field<1, ID>::type t1,
	    typename field<2, ID>::type t2,
	    typename field<3, ID>::type t3,
	    typename field<4, ID>::type t4)
  {
    send(to, pack<ID>(t0, t1, t2, t3, t4));
  }

  template <MSGID ID>
  void send(const PID &to,
	    typename field<0, ID>::type t0,
	    typename field<1, ID>::type t1,
	    typename field<2, ID>::type t2,
	    typename field<3, ID>::type t3,
	    typename field<4, ID>::type t4,
	    typename field<5, ID>::type t5)
  {
    send(to, pack<ID>(t0, t1, t2, t3, t4, t5));
  }

  template <MSGID ID>
  void send(const PID &to,
	    typename field<0, ID>::type t0,
	    typename field<1, ID>::type t1,
	    typename field<2, ID>::type t2,
	    typename field<3, ID>::type t3,
	    typename field<4, ID>::type t4,
	    typename field<5, ID>::type t5,
	    typename field<6, ID>::type t6)
  {
    send(to, pack<ID>(t0, t1, t2, t3, t4, t5, t6));
  }

  template <MSGID ID>
  void send(const PID &to,
	    typename field<0, ID>::type t0,
	    typename field<1, ID>::type t1,
	    typename field<2, ID>::type t2,
	    typename field<3, ID>::type t3,
	    typename field<4, ID>::type t4,
	    typename field<5, ID>::type t5,
	    typename field<6, ID>::type t6,
	    typename field<7, ID>::type t7)
  {
    send(to, pack<ID>(t0, t1, t2, t3, t4, t5, t6, t7));
  }

  template <MSGID ID>
  void send(const PID &to,
	    typename field<0, ID>::type t0,
	    typename field<1, ID>::type t1,
	    typename field<2, ID>::type t2,
	    typename field<3, ID>::type t3,
	    typename field<4, ID>::type t4,
	    typename field<5, ID>::type t5,
	    typename field<6, ID>::type t6,
	    typename field<7, ID>::type t7,
	    typename field<8, ID>::type t8)
  {
    send(to, pack<ID>(t0, t1, t2, t3, t4, t5, t6, t7, t8));
  }

  template <MSGID ID>
  void send(const PID &to,
	    typename field<0, ID>::type t0,
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
    send(to, pack<ID>(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9));
  }

  MSGID receive()
  {
    return receive(0);
  }

  MSGID receive(double secs)
  {
    return P::receive(secs);
  }

  template <MSGID ID>
  void receive(const ::boost::variant<
	       typename field<0, ID>::type &,
	       const typename field<0, ID>::type &,
	       const ::boost::tuples::detail::swallow_assign &> &v0)
  {
    if (typename field<0, ID>::type *t =
	::boost::get<typename field<0, ID>::type &>(&v0)) {
      // TODO(benh): Set this formal.
    } else if (const typename field<0, ID>::type *t =
	       ::boost::get<const typename field<0, ID>::type &>(&v0)) {
      // TODO(benh): Match this actual.
    } else {
      // TODO(benh): Ignore this parameter.
    }
  }

  template <MSGID ID>
  MSGID call(const PID &to, const tuple<ID> &r)
  {
    send(to, r);
    return receive();
  }

  template <MSGID ID>
  MSGID call(const PID &to, const tuple<ID> &r, double secs)
  {
    send(to, r);
    return receive(secs);
  }

  template <MSGID ID>
  MSGID call(const PID &to)
  {
    return call(to, pack<ID>());
  }

  template <MSGID ID>
  MSGID call(const PID &to, typename field<0, ID>::type t0)
  {
    return call(to, pack<ID>(t0));
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1)
  {
    return call(to, pack<ID>(t0, t1));
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2)
  {
    return call(to, pack<ID>(t0, t1, t2));
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3)
  {
    return call(to, pack<ID>(t0, t1, t2, t3));
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3,
	     typename field<4, ID>::type t4)
  {
    return call(to, pack<ID>(t0, t1, t2, t3, t4));
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3,
	     typename field<4, ID>::type t4,
	     typename field<5, ID>::type t5)
  {
    return call(to, pack<ID>(t0, t1, t2, t3, t4, t5));
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3,
	     typename field<4, ID>::type t4,
	     typename field<5, ID>::type t5,
	     typename field<6, ID>::type t6)
  {
    return call(to, pack<ID>(t0, t1, t2, t3, t4, t5, t6));
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3,
	     typename field<4, ID>::type t4,
	     typename field<5, ID>::type t5,
	     typename field<6, ID>::type t6,
	     typename field<7, ID>::type t7)
  {
    return call(to, pack<ID>(t0, t1, t2, t3, t4, t5, t6, t7));
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3,
	     typename field<4, ID>::type t4,
	     typename field<5, ID>::type t5,
	     typename field<6, ID>::type t6,
	     typename field<7, ID>::type t7,
	     typename field<8, ID>::type t8)
  {
    return call(to, pack<ID>(t0, t1, t2, t3, t4, t5, t6, t7, t8));
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
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
    return call(to, pack<ID>(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9));
  }

  template <MSGID ID>
  MSGID call(const PID &to, double secs)
  {
    return call(to, pack<ID>(), secs);
  }

  template <MSGID ID>
  MSGID call(const PID &to, typename field<0, ID>::type t0, double secs)
  {
    return call(to, pack<ID>(t0), secs);
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     double secs)
  {
    return call(to, pack<ID>(t0, t1), secs);
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     double secs)
  {
    return call(to, pack<ID>(t0, t1, t2), secs);
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3,
	     double secs)
  {
    return call(to, pack<ID>(t0, t1, t2, t3), secs);
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3,
	     typename field<4, ID>::type t4,
	     double secs)
  {
    return call(to, pack<ID>(t0, t1, t2, t3, t4), secs);
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3,
	     typename field<4, ID>::type t4,
	     typename field<5, ID>::type t5,
	     double secs)
  {
    return call(to, pack<ID>(t0, t1, t2, t3, t4, t5), secs);
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3,
	     typename field<4, ID>::type t4,
	     typename field<5, ID>::type t5,
	     typename field<6, ID>::type t6,
	     double secs)
  {
    return call(to, pack<ID>(t0, t1, t2, t3, t4, t5, t6), secs);
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3,
	     typename field<4, ID>::type t4,
	     typename field<5, ID>::type t5,
	     typename field<6, ID>::type t6,
	     typename field<7, ID>::type t7,
	     double secs)
  {
    return call(to, pack<ID>(t0, t1, t2, t3, t4, t5, t6, t7), secs);
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3,
	     typename field<4, ID>::type t4,
	     typename field<5, ID>::type t5,
	     typename field<6, ID>::type t6,
	     typename field<7, ID>::type t7,
	     typename field<8, ID>::type t8,
	     double secs)
  {
    return call(to, pack<ID>(t0, t1, t2, t3, t4, t5, t6, t7, t8), secs);
  }

  template <MSGID ID>
  MSGID call(const PID &to,
	     typename field<0, ID>::type t0,
	     typename field<1, ID>::type t1,
	     typename field<2, ID>::type t2,
	     typename field<3, ID>::type t3,
	     typename field<4, ID>::type t4,
	     typename field<5, ID>::type t5,
	     typename field<6, ID>::type t6,
	     typename field<7, ID>::type t7,
	     typename field<8, ID>::type t8,
	     typename field<9, ID>::type t9,
	     double secs)
  {
    return call(to, pack<ID>(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9), secs);
  }

public:
  template <MSGID ID>
  static void post(const PID &to, const tuple<ID> &r)
  {
    std::ostringstream os;

    process::serialization::serializer s(os);

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

    Process::post(to, ID, data.data(), data.size());    
  }

  template <MSGID ID>
  static string tupleToString(const tuple<ID> &r)
  {
    std::ostringstream os;

    process::serialization::serializer s(os);

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

    return os.str();
  }
};
