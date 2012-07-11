#ifndef __FOREACH_HPP__
#define __FOREACH_HPP__

#include <boost/foreach.hpp>

#include <boost/tuple/tuple.hpp>


#define BOOST_FOREACH_PAIR(VARFIRST, VARSECOND, COL)                                            \
    BOOST_FOREACH_PREAMBLE()                                                                    \
    if (boost::foreach_detail_::auto_any_t _foreach_col = BOOST_FOREACH_CONTAIN(COL)) {} else   \
    if (boost::foreach_detail_::auto_any_t _foreach_cur = BOOST_FOREACH_BEGIN(COL)) {} else     \
    if (boost::foreach_detail_::auto_any_t _foreach_end = BOOST_FOREACH_END(COL)) {} else       \
    for (bool _foreach_continue = true, _foreach_onetime = true;                                \
              _foreach_continue && !BOOST_FOREACH_DONE(COL);                                    \
              _foreach_continue ? BOOST_FOREACH_NEXT(COL) : (void)0)                            \
        if  (boost::foreach_detail_::set_false(_foreach_onetime)) {} else                       \
        for (VARFIRST = BOOST_FOREACH_DEREF(COL).first;                                         \
	     !_foreach_onetime;                                                                 \
	     _foreach_onetime = true)                                                           \
            if  (boost::foreach_detail_::set_false(_foreach_continue)) {} else                  \
            for (VARSECOND = BOOST_FOREACH_DEREF(COL).second;                                   \
		 !_foreach_continue;                                                            \
		 _foreach_continue = true)

#define foreach BOOST_FOREACH
#define foreachpair BOOST_FOREACH_PAIR

#define foreachkey(VAR, COL)                    \
  foreach (boost::tuples::tie(VAR, boost::tuples::ignore), COL)

#define foreachvalue(VAR, COL)                  \
  foreach (boost::tuples::tie(boost::tuples::ignore, VAR), COL)

#endif // __FOREACH_HPP__
