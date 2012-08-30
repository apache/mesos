#ifndef __STOUT_FOREACH_HPP__
#define __STOUT_FOREACH_HPP__

#include <boost/foreach.hpp>

#include <boost/tuple/tuple.hpp>


// TODO(bmahler): Do not name this namespace 'internal', it causes the namesapce
// to be nested within the process namespace, this should be investigated and
// fixed at some point.
namespace hidden {

// NOTE: This is a duplicate of boosts swallow_assign in boost/tuple_basic.hpp
// We've added this copy because of a change in boost that doesn't allow us to
// use detail::ignore with our foreachpair any longer.
struct swallow_assign {
  template<typename T>
  swallow_assign const& operator=(const T&) const {
    return *this;
  }
};

swallow_assign const ignore = swallow_assign();

}  // namespace hidden

#define BOOST_FOREACH_PAIR(VARFIRST, VARSECOND, COL)                                            \
    BOOST_FOREACH_PREAMBLE()                                                                    \
    if (boost::foreach_detail_::auto_any_t BOOST_FOREACH_ID(_foreach_col) = BOOST_FOREACH_CONTAIN(COL)) {} else   \
    if (boost::foreach_detail_::auto_any_t BOOST_FOREACH_ID(_foreach_cur) = BOOST_FOREACH_BEGIN(COL)) {} else     \
    if (boost::foreach_detail_::auto_any_t BOOST_FOREACH_ID(_foreach_end) = BOOST_FOREACH_END(COL)) {} else       \
    for (bool BOOST_FOREACH_ID(_foreach_continue) = true, BOOST_FOREACH_ID(_foreach_onetime) = true;                                \
              BOOST_FOREACH_ID(_foreach_continue) && !BOOST_FOREACH_DONE(COL);                                    \
              BOOST_FOREACH_ID(_foreach_continue) ? BOOST_FOREACH_NEXT(COL) : (void)0)                            \
        if  (boost::foreach_detail_::set_false(BOOST_FOREACH_ID(_foreach_onetime))) {} else                       \
        for (VARFIRST = BOOST_FOREACH_DEREF(COL).first;                                         \
	     !BOOST_FOREACH_ID(_foreach_onetime);                                                                 \
	     BOOST_FOREACH_ID(_foreach_onetime) = true)                                                           \
            if  (boost::foreach_detail_::set_false(BOOST_FOREACH_ID(_foreach_continue))) {} else                  \
            for (VARSECOND = BOOST_FOREACH_DEREF(COL).second;                                   \
		 !BOOST_FOREACH_ID(_foreach_continue);                                                            \
		 BOOST_FOREACH_ID(_foreach_continue) = true)

#define foreach BOOST_FOREACH
#define foreachpair BOOST_FOREACH_PAIR

#define foreachkey(VAR, COL)                    \
  foreachpair (VAR, hidden::ignore, COL)

#define foreachvalue(VAR, COL)                  \
  foreachpair (hidden::ignore, VAR, COL)

#endif // __STOUT_FOREACH_HPP__
