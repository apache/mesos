#!/bin/sh

# Check that the JavaException framework crashes and prints an ArrayIndexOutOfBoundsExcpetion. This is a test to be sure that Java excpetions are getting propagated, which is an issue with Swig generated Java libraries. The grep should returns 0.
$MESOS_HOME/bin/examples/java/test_exception_framework local 2>&1 | grep "ArrayIndexOutOfBoundsException"

exit $?
