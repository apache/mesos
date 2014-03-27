template <MSGID ID> class tuple;

#undef IDENTITY
#define IDENTITY(...) __VA_ARGS__

#undef TUPLE
#define TUPLE(ID, types)                                                \
  template <> class tuple<ID> : public boost::tuple<IDENTITY types>     \
  {                                                                     \
  public:                                                               \
    explicit tuple(const boost::tuple<IDENTITY types> &t)               \
      : boost::tuple<IDENTITY types>(t) {}                              \
                                                                        \
    explicit tuple(const std::string &data)                             \
    {                                                                   \
      std::istringstream is(data);                                      \
      process::tuples::deserializer d(is);                              \
      deserialize(d, *this);                                            \
    }                                                                   \
                                                                        \
    operator std::string () const                                       \
    {                                                                   \
      std::ostringstream os;                                            \
      process::tuples::serializer s(os);                                \
      serialize(s, *this);                                              \
      return os.str();                                                  \
    }                                                                   \
  }


inline void serialize(process::tuples::serializer &s,
                      const boost::tuples::null_type &)
{
}


template <typename H, typename T>
inline void serialize(process::tuples::serializer &s,
                      const boost::tuples::cons<H, T> &c)
{
  s & c.get_head();
  serialize(s, c.get_tail());
}


inline void deserialize(process::tuples::deserializer &d,
                        const boost::tuples::null_type &)
{
}


template <typename H, typename T>
inline void deserialize(process::tuples::deserializer &d,
                        boost::tuples::cons<H, T> &c)
{
  d & c.get_head();
  deserialize(d, c.get_tail());
}


template <MSGID ID>
tuple<ID> pack()
{
  return tuple<ID>(::boost::make_tuple());
}


template <MSGID ID, typename T0>
tuple<ID> pack(const T0 &t0)
{
  return tuple<ID>(::boost::make_tuple(t0));
}


template <MSGID ID, typename T0, typename T1>
tuple<ID> pack(const T0 &t0, const T1 &t1)
{
  return tuple<ID>(::boost::make_tuple(t0, t1));
}


template <MSGID ID, typename T0, typename T1, typename T2>
tuple<ID> pack(const T0 &t0, const T1 &t1, const T2 &t2)
{
  return tuple<ID>(::boost::make_tuple(t0, t1, t2));
}


template <MSGID ID,
          typename T0, typename T1, typename T2, typename T3>
tuple<ID> pack(const T0 &t0, const T1 &t1, const T2 &t2, const T3 &t3)
{
  return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3));
}


template <MSGID ID,
          typename T0, typename T1, typename T2, typename T3, typename T4>
tuple<ID> pack(const T0 &t0, const T1 &t1, const T2 &t2, const T3 &t3,
               const T4 &t4)
{
  return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3, t4));
}


template <MSGID ID,
          typename T0, typename T1, typename T2, typename T3, typename T4,
          typename T5>
tuple<ID> pack(const T0 &t0, const T1 &t1, const T2 &t2, const T3 &t3,
               const T4 &t4, const T5 &t5)
{
  return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5));
}


template <MSGID ID,
          typename T0, typename T1, typename T2, typename T3, typename T4,
          typename T5, typename T6>
tuple<ID> pack(const T0 &t0, const T1 &t1, const T2 &t2, const T3 &t3,
               const T4 &t4, const T5 &t5, const T6 &t6)
{
  return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5, t6));
}


template <MSGID ID,
          typename T0, typename T1, typename T2, typename T3, typename T4,
          typename T5, typename T6, typename T7>
tuple<ID> pack(const T0 &t0, const T1 &t1, const T2 &t2, const T3 &t3,
               const T4 &t4, const T5 &t5, const T6 &t6, const T7 &t7)
{
  return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5, t6, t7));
}


template <MSGID ID,
          typename T0, typename T1, typename T2, typename T3, typename T4,
          typename T5, typename T6, typename T7, typename T8>
tuple<ID> pack(const T0 &t0, const T1 &t1, const T2 &t2, const T3 &t3,
               const T4 &t4, const T5 &t5, const T6 &t6, const T7 &t7,
               const T8 &t8)
{
  return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5, t6, t7, t8));
}


template <MSGID ID,
          typename T0, typename T1, typename T2, typename T3, typename T4,
          typename T5, typename T6, typename T7, typename T8, typename T9>
tuple<ID> pack(const T0 &t0, const T1 &t1, const T2 &t2, const T3 &t3,
               const T4 &t4, const T5 &t5, const T6 &t6, const T7 &t7,
               const T8 &t8, const T9 &t9)
{
  return tuple<ID>(::boost::make_tuple(t0, t1, t2, t3, t4, t5, t6, t7, t8, t9));
}


template <MSGID ID>
tuple<ID> unpack(const std::string &data)
{
  return tuple<ID>(data);
}


template <MSGID ID, int N>
typename boost::tuples::element<N, tuple<ID> >::type unpack(
  const std::string &data)
{
  return boost::tuples::get<N>(unpack<ID>(data));
}
