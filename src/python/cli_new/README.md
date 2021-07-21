# Mesos CLI

## Prerequisites

Make sure you have python 3.6 or newer installed
on your system before you begin.

## Getting Started

Once you you have the prerequisites installed, simply run the
`bootstrap` script from this directory to set up a virtual
environment using python 3 and start running the tool.

```
$ ./bootstrap

...

Setup complete!

To begin working, simply activate your virtual environment,
run the CLI, and then deactivate the virtual environment
when you are done.

    $ source activate
    $ mesos <command> [<args>...]
    $ source deactivate
```

You can also run the `bootstrap` script from any directory
and specify the `VIRTUALENV_DIRECTORY` to set where it
should be created.

```
$ VIRTUALENV_DIRECTORY=~/.mesos-cli-venv
$ ${MESOS_DIR}/src/python/cli_new/bootstrap

...

Setup complete!

To begin working, simply activate your virtual environment,
run the CLI, and then deactivate the virtual environment
when you are done.

    $ source ~/.mesos-cli-venv/bin/activate
    $ source ~/.mesos-cli-venv/bin/postactivate
    $ mesos <command> [<args>...]
    $ source ~/.mesos-cli-venv/bin/predeactivate
    $ deactivate


The postactivate and predeactivate files set up autocompletion.
Add the mesos binary parent directory
${MESOS_DIR}/src/python/cli_new/bin/
to your path, export it, and source
${MESOS_DIR}/src/python/cli_new/mesos.bash_completion
to skip these two steps in the future.
```

## Running tests

To run the unit tests developed for the Mesos CLI, use
`mesos-cli-tests`:

```
$ ./bootstrap

...

Setup complete!

To begin working, simply activate your virtual environment,
run the CLI, and then deactivate the virtual environment
when you are done.

    $ source activate
    $ mesos-cli-tests

Running the Mesos CLI unit tests

...

OK
```


## Setting up your configuration

In order to use this tool, you will need to create a
configuration file in your home directory under
`~/.mesos/config.toml`. A template for this config can be
seen below:

```
# The `plugins` is an array listing the absolute paths of the
# plugins you want to add to the CLI.
plugins = [
  "</absolute/path/to/plugin-1/directory>",
  "</absolute/path/to/plugin-2/directory>"
]

# The `master` is a field that has to be composed of an
# `address` or `zookeeper` field, but not both. For example:
[master]
  address = "10.10.0.30:5050"
  principal = "username"
  secret = "password"

  # The `zookeeper` field has an `addresses` array and a `path` field.
  # [master.zookeeper]
  #   addresses = [
  #     "10.10.0.31:5050",
  #     "10.10.0.32:5050",
  #     "10.10.0.33:5050"
  #   ]
  #   path = "/mesos"

[agent]
  ssl = true
  ssl_verify = false
  principal = "username"
  secret = "password"
  timeout = 5
```

You can override the location of this configuration file using
the environment variable `MESOS_CLI_CONFIG`.
