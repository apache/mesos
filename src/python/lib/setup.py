# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Setup script for the mesos package
"""

import os
import re

from setuptools import find_packages, setup


def read_requirements(filename="requirements.in"):
    """
    Load list of dependent packages for the mesos package.

    :param filename: filename to load requirements from
    :type filename: str
    :rtype: list[str]
    """
    with open(filename) as f:
        return f.readlines()

def find_version(*relative_path_parts):
    """
    Find the version string in a file relative to the current directory.

    :param relative_path_parts: list of path parts relative
                                to the current directory where
                                the file containing the __version__
                                string lives
    :type relative_path_parts: list[str]
    :rtype: str
    """
    currdir = os.path.abspath(os.path.dirname(__file__))
    version_file = os.path.join(currdir, *relative_path_parts)

    with open(version_file, 'r') as f:
        match = re.search(r"^__version__ = ['\"]([^'\"]*)['\"]", f.read(), re.M)
        if not match:
            raise RuntimeError("Unable to find version string.")
        return match.group(1)

setup(
    author='Apache Mesos',
    author_email='dev@mesos.apache.org',
    description='Client library for Mesos http rest api',
    include_package_data=True,
    install_requires=read_requirements(),
    license='apache',
    name='mesos',
    packages=find_packages(),
    version=find_version('mesos', '__init__.py'),
    zip_safe=False,
)
