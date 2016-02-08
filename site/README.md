# Apache Mesos website generator
This will generate the site available at http://mesos.apache.org. The content
within the publish folder will be the actual deployed site.


## Setup

		gem install bundler
		bundle install


## Generating the site
Running `rake` will download the latest Apache Mesos documentation contained
in the `docs` folder, integrate them into the site, and generate all other
files within the source folder.

		# First generate the help endpoint documentation. Running
		# `make check` is needed to generate the latest version of
		# the master and slave binaries.
                make check -jN GTEST_FILTER=""
		../support/generate-help-site.py
		rake


## Development
To live edit the site run `rake dev` and then open a browser window to
http://localhost:4567/ . Any change you make to the sources dir will
be shown on the local dev site immediately. Errors will be shown in the
console you launched `rake dev` within.


## Publishing the Site
The website uses svnpubsub. The publish folder contains the websites content
and when committed to the svn repository it will be automatically deployed to
the live site.


## Other available tasks

		rake build        # Build the website from source
		rake clean        # Remove any temporary products
		rake clobber      # Remove any generated file
		rake dev          # Run the site in development mode
		rake update_docs  # Update the latest docs from the Apache Mesos codebase
		rake doxygen			# Update doxygen from C++ source files
		rake javadoc			# Update javadocs from java source files
