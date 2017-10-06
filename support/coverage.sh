#!/usr/bin/env bash

set -e

which lcov 1>/dev/null 2>&1
if [ $? != 0 ]; then
	echo "lcov is required to generate the coverage report"
	exit 1
fi

if [[ -z $MAKE ]]; then
	MAKE=make
fi

if [[ -z $GTEST_FILTER ]]; then
	GTEST_FILTER='*'
fi

pushd build

# Reconfigure with gcov support.
CXXFLAGS="-pg --coverage" CFLAGS="-pg --coverage" LDFLAGS="-lgcov" ../configure --disable-optimize --disable-libtool-wrappers

# Ensure no old data is in the tree.
find -name \*.gcda | xargs rm
find -name \*.gcno | xargs rm

# Generate gcov output.
${MAKE}

lcov --directory . --zerocounters
${MAKE} check GTEST_FILTER=${GTEST_FILTER}

# Generate report.
lcov --directory . -c -o mesos_test.info

# Remove output for external libraries and generated files.
LCOV_FILTERS="/usr/include/*"
LCOV_FILTERS+=" /usr/lib/jvm/*"
LCOV_FILTERS+=" mesos/build/*"
LCOV_FILTERS+=" build/3rdparty/concurrentqueue*"
LCOV_FILTERS+=" build/3rdparty/setuptools-*"
LCOV_FILTERS+=" build/3rdparty/leveldb*"
LCOV_FILTERS+=" build/3rdparty/zookeeper-*"
LCOV_FILTERS+=" */boost-*"
LCOV_FILTERS+=" */elfio-*"
LCOV_FILTERS+=" */glog-*"
LCOV_FILTERS+=" */gmock-*"
LCOV_FILTERS+=" */picojson-*"
LCOV_FILTERS+=" */protobuf-*"

for f in $LCOV_FILTERS; do
	lcov --remove mesos_test.info $f -o mesos_test.info
done

# Generate HTML report.
genhtml -o ./test_coverage -t "mesos test coverage" --num-spaces 2 mesos_test.info

popd
