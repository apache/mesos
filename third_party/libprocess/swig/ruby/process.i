%module(directors="1") process

%include "typemaps.i"

%{
#include <process.hpp>
#define SWIG_STD_NOASSIGN_STL
%}


%import <stdint.i>
%import <std_string.i>


%ignore schedule(void *);


/* Make it possible to inherit from Process in target language. */
%feature("director") Process;

%apply size_t *OUTPUT { size_t *length };

%include <process.hpp>
