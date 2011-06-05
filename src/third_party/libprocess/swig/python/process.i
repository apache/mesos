%module(directors="1") process

%include "typemaps.i"

%{
#include <process.hpp>
#define SWIG_STD_NOASSIGN_STL
%}


%pythoncode %{
processes = {}
%}

%pythonprepend Process::spawn(Process *) %{
  process, = args
  processes[process.getPID().pipe] = process
%}

%pythonappend Process::wait(PID) %{
  pid, = args
  del processes[pid.pipe]
%}


%import <stdint.i>
%import <std_string.i>


%ignore schedule(void *);


/* Make it possible to inherit from Process in target language. */
%feature("director") Process;

%apply size_t *OUTPUT { size_t *length };

%rename(_self) Process::self();
%rename(_from) Process::from();

%include <process.hpp>
