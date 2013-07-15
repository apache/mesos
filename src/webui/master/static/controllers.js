'use strict';

var slaves = [];


// Table Object.
//   selected_column: column predicate for the selected column.
//   reverse:         boolean indicating sort order.
function Table(selected_column) {
  if (this instanceof Table) {
    this.selected_column = selected_column;
    this.reverse = true;
  } else {
    return new Table(selected_column);
  }
}


function hasSelectedText () {
  if (window.getSelection) {  // All browsers except IE before version 9.
    var range = window.getSelection();
    return range.toString().length > 0;
  }
  return false;
}


// Returns a curried function for returning the HTML 'class=' tag
// attribute value for sorting table columns in the provided scope.
function columnClass($scope) {
  // For the given table column, this behaves as follows:
  // Column unselected            : 'unselected'
  // Column selected / descending : 'descending'
  // Column selected / ascending  : 'ascending'
  return function(table, column) {
    if ($scope.tables[table].selected_column === column) {
      if ($scope.tables[table].reverse) {
        return 'descending';
      } else {
        return 'ascending';
      }
    }
    return 'unselected';
  }
}


// Returns a curried function to be called when a table column is clicked
// in the provided scope.
function selectColumn($scope) {
  // Assigns the given table column as the sort column, flipping the
  // sort order if the sort column has not changed.
  return function(table, column) {
    if ($scope.tables[table].selected_column === column) {
      $scope.tables[table].reverse = !$scope.tables[table].reverse;
    } else {
      $scope.tables[table].reverse = true;
    }
    $scope.tables[table].selected_column = column;
  }
}


// Invokes the pailer for the specified host and path using the
// specified window_title.
function pailer(host, path, window_title) {
  var url = 'http://' + host + '/files/read.json?path=' + path;
  var pailer =
    window.open('/static/pailer.html', url, 'width=580px, height=700px');

  // Need to use window.onload instead of document.ready to make
  // sure the title doesn't get overwritten.
  pailer.onload = function() {
    pailer.document.title = window_title + ' (' + host + ')';
  }
}


function updateInterval(num_slaves) {
  // TODO(bmahler): Increasing the update interval for large clusters
  // is done purely to mitigate webui performance issues. Ideally we can
  // keep a consistently fast rate for updating statistical information.
  // For the full system state updates, it may make sense to break
  // it up using pagination and/or splitting the endpoint.
  if (num_slaves < 10) {
    return 3000;
  } else if (num_slaves < 50) {
    return 4000;
  } else if (num_slaves < 100) {
    return 5000;
  } else if (num_slaves < 500) {
    return 10000;
  } else if (num_slaves < 1000) {
    return 20000;
  } else if (num_slaves < 5000) {
    return 30000;
  } else {
    return 60000;
  }
}


// Update the outermost scope with the new state.
function update($scope, $timeout, data) {
  // Don't do anything if the data hasn't changed.
  if ($scope.data == data) {
    return true; // Continue polling.
  }

  $scope.state = $.parseJSON(data);

  // Determine if there is a leader (and redirect if not the leader).
  if (!$scope.state.leader) {
    $("#no-leader-alert").show();
  } else {
    $("#no-leader-alert").hide();

    // Redirect if we aren't the leader.
    if ($scope.state.leader != $scope.state.pid) {
      $scope.redirect = 6000;
      $scope.leader = $scope.state.leader.split("@")[1];
      $("#not-leader-alert").show();

      var countdown = function() {
        if ($scope.redirect == 0) {
          // TODO(benh): Use '$window'.
          window.location = 'http://' + $scope.leader;
        } else {
          $scope.redirect = $scope.redirect - 1000;
          $timeout(countdown, 1000);
        }
      };
      countdown();
      return false; // Don't continue polling.
    }
  }

  // A cluster is named if the state returns a non-empty string name.
  // Track whether this cluster is named in a Boolean for display purposes.
  $scope.clusterNamed = !!$scope.state.cluster;

  // Check for selected text, and allow up to 20 seconds to pass before
  // potentially wiping the user highlighted text.
  // TODO(bmahler): This is to avoid the annoying loss of highlighting when
  // the tables update. Once we can have tighter granularity control on the
  // angular.js dynamic table updates, we should remove this hack.
  $scope.time_since_update += $scope.delay;

  if (hasSelectedText() && $scope.time_since_update < 20000) {
    return true;
  }

  $scope.data = data;

  // Update the maps.
  $scope.slaves = {};
  $scope.frameworks = {};
  $scope.offers = {};
  $scope.completed_frameworks = {};

  _.each($scope.state.slaves, function(slave) {
    $scope.slaves[slave.id] = slave;
  });

  _.each($scope.state.frameworks, function(framework) {
    $scope.frameworks[framework.id] = framework;
    _.each(framework.offers, function(offer) {
      $scope.offers[offer.id] = offer;
    });
  });

  _.each($scope.state.completed_frameworks, function(framework) {
    $scope.completed_frameworks[framework.id] = framework;
  });

  // Update the stats.
  $scope.cluster = $scope.state.cluster;
  $scope.total_cpus = 0;
  $scope.total_mem = 0;
  $scope.used_cpus = 0;
  $scope.used_mem = 0;
  $scope.offered_cpus = 0;
  $scope.offered_mem = 0;

  $scope.staged_tasks = $scope.state.staged_tasks;
  $scope.started_tasks = $scope.state.started_tasks;
  $scope.finished_tasks = $scope.state.finished_tasks;
  $scope.killed_tasks = $scope.state.killed_tasks;
  $scope.failed_tasks = $scope.state.failed_tasks;
  $scope.lost_tasks = $scope.state.lost_tasks;

  $scope.activated_slaves = $scope.state.activated_slaves;
  $scope.deactivated_slaves = $scope.state.deactivated_slaves;

  _.each($scope.slaves, function(slave) {
    $scope.total_cpus += slave.resources.cpus;
    $scope.total_mem += slave.resources.mem;
  });

  _.each($scope.frameworks, function(framework) {
      $scope.used_cpus += framework.resources.cpus;
      $scope.used_mem += framework.resources.mem;
      $scope.active_tasks += framework.tasks.length;
      $scope.completed_tasks += framework.completed_tasks.length;

      framework.cpus_share = 0;
      if ($scope.total_cpus > 0) {
        framework.cpus_share = framework.resources.cpus / $scope.total_cpus;
      }

      framework.mem_share = 0;
      if ($scope.total_mem > 0) {
        framework.mem_share = framework.resources.mem / $scope.total_mem;
      }

      framework.max_share = Math.max(framework.cpus_share, framework.mem_share);

      // If the executor ID is empty, this is a command executor with an
      // internal executor ID generated from the task ID.
      // TODO(brenden): Remove this once
      // https://issues.apache.org/jira/browse/MESOS-527 is fixed.
      _.each(framework.tasks, function(task) {
        if (!task.executor_id) {
          task.executor_id = task.id;
        }
      });
      _.each(framework.completed_tasks, function(task) {
        if (!task.executor_id) {
          task.executor_id = task.id;
        }
      });
  });

  _.each($scope.offers, function(offer) {
    $scope.offered_cpus += offer.resources.cpus;
    $scope.offered_mem += offer.resources.mem;
    offer.framework_name = $scope.frameworks[offer.framework_id].name;
    offer.hostname = $scope.slaves[offer.slave_id].hostname;
  });

  $scope.used_cpus -= $scope.offered_cpus;
  $scope.used_mem -= $scope.offered_mem;

  $scope.idle_cpus = $scope.total_cpus - ($scope.offered_cpus + $scope.used_cpus);
  $scope.idle_mem = $scope.total_mem - ($scope.offered_mem + $scope.used_mem);

  $scope.time_since_update = 0;
  $.event.trigger('state_updated');

  return true; // Continue polling.
}


// Main controller that can be used to handle "global" events. E.g.,:
//     $scope.$on('$afterRouteChange', function() { ...; });
//
// In addition, the MainCntl encapsulates the "view", allowing the
// active controller/view to easily access anything in scope (e.g.,
// the state).
function MainCntl($scope, $http, $route, $routeParams, $location, $timeout) {
  // Turn off the loading gif, turn on the navbar.
  $("#loading").hide();
  $("#navbar").show();

  // Adding bindings into scope so that they can be used from within
  // AngularJS expressions.
  $scope._ = _;
  $scope.stringify = JSON.stringify;
  $scope.encodeURIComponent = encodeURIComponent;
  $scope.basename = function(path) {
    // This is only a basic version of basename that handles the cases we care
    // about, rather than duplicating unix basename functionality perfectly.
    if (path === '/') {
      return path;  // Handle '/'.
    }

    // Strip a trailing '/' if present.
    if (path.length > 0 && path.lastIndexOf('/') === (path.length - 1)) {
      path = path.substr(0, path.length - 1);
    }
    return path.substr(path.lastIndexOf('/') + 1);
  };

  $scope.$location = $location;
  $scope.delay = 2000;
  $scope.retry = 0;
  $scope.time_since_update = 0;

  var poll = function() {
    $http.get('master/state.json',
              {transformResponse: function(data) { return data; }})
      .success(function(data) {
        if (update($scope, $timeout, data)) {
          $scope.delay = updateInterval(_.size($scope.slaves));
          $timeout(poll, $scope.delay);
        }
      })
      .error(function() {
        if ($scope.delay >= 128000) {
          $scope.delay = 2000;
        } else {
          $scope.delay = $scope.delay * 2;
        }

        $scope.retry = $scope.delay;
        var countdown = function() {
          if ($scope.retry === 0) {
            $scope.errorModalClose();
          } else {
            $scope.retry = $scope.retry - 1000;
            $scope.countdown = $timeout(countdown, 1000);
          }
        };

        countdown();
        $scope.errorModalOpen = true;
      });
  };

  // Make it such that everytime we hide the error-modal, we stop the
  // countdown and restart the polling.
  $scope.errorModalClose = function() {
    $scope.errorModalOpen = false;

    if ($scope.countdown != null) {
      if ($timeout.cancel($scope.countdown)) {
        // Restart since they cancelled the countdown.
        $scope.delay = 2000;
      }
    }

    // Start polling again, but do it asynchronously (and wait at
    // least a second because otherwise the error-modal won't get
    // properly shown).
    $timeout(poll, 1000);
  };

  poll();
}


function HomeCtrl($dialog, $scope) {
  setNavbarActiveTab('home');

  $scope.tables = {};
  $scope.tables['frameworks'] = new Table('id');
  $scope.tables['slaves'] = new Table('id');
  $scope.tables['offers'] = new Table('id');
  $scope.tables['completed_frameworks'] = new Table('id');

  $scope.columnClass = columnClass($scope);
  $scope.selectColumn = selectColumn($scope);

  $scope.log = function($event) {
    if (!$scope.state.log_dir) {
      $dialog.messageBox(
        'Logging to a file is not enabled',
        "Set the 'log_dir' option if you wish to access the logs.",
        [{label: 'Continue'}]
      ).open();
    } else {
      pailer(
          $scope.$location.host() + ':' + $scope.$location.port(),
          '/master/log',
          'Mesos Master');
    }
  };
}


function DashboardCtrl($scope) {
  setNavbarActiveTab('dashboard');

  var context = cubism.context()
    .step(1000)
    .size(1440);

  // Create a "cpus" horizon.
  horizons.create(context, "cpus", random(context, "cpus"), [0, 10], "cpus");

  // Create a "mem" horizon.
  horizons.create(context, "mem", random(context, "mem"), [0, 10], "mb");

  // Do any cleanup before we change the route.
  $scope.$on('$beforeRouteChange', function() { context.stop(); });
}


function FrameworksCtrl($scope) {
  setNavbarActiveTab('frameworks');

  $scope.tables = {};
  $scope.tables['frameworks'] = new Table('id');

  $scope.columnClass = columnClass($scope);
  $scope.selectColumn = selectColumn($scope);
}


function FrameworkCtrl($scope, $routeParams) {
  setNavbarActiveTab('frameworks');

  $scope.tables = {};
  $scope.tables['active_tasks'] = new Table('id');
  $scope.tables['completed_tasks'] = new Table('id');

  $scope.columnClass = columnClass($scope);
  $scope.selectColumn = selectColumn($scope);

  var update = function() {
    if ($routeParams.id in $scope.completed_frameworks) {
      $scope.framework = $scope.completed_frameworks[$routeParams.id];
      $scope.alert_message = 'This framework has terminated!';
      $('#alert').show();
      $('#framework').show();
    } else if ($routeParams.id in $scope.frameworks) {
      $scope.framework = $scope.frameworks[$routeParams.id];
      $('#framework').show();
    } else {
      $scope.alert_message = 'No framework found with ID: ' + $routeParams.id;
      $('#alert').show();
    }
  }

  if ($scope.state) {
    update();
  }

  $(document).on('state_updated', update);
  $scope.$on('$beforeRouteChange', function() {
    $(document).off('state_updated', update);
  });
}


function SlavesCtrl($scope) {
  setNavbarActiveTab('slaves');

  $scope.tables = {};
  $scope.tables['slaves'] = new Table('id');

  $scope.columnClass = columnClass($scope);
  $scope.selectColumn = selectColumn($scope);
}


function SlaveCtrl($dialog, $scope, $routeParams, $http, $q) {
  setNavbarActiveTab('slaves');

  $scope.slave_id = $routeParams.slave_id;

  $scope.tables = {};
  $scope.tables['frameworks'] = new Table('id');
  $scope.tables['completed_frameworks'] = new Table('id');

  $scope.columnClass = columnClass($scope);
  $scope.selectColumn = selectColumn($scope);

  var update = function() {
    if (!($routeParams.slave_id in $scope.slaves)) {
      $scope.alert_message = 'No slave found with ID: ' + $routeParams.slave_id;
      $('#alert').show();
      return;
    }

    var pid = $scope.slaves[$routeParams.slave_id].pid;
    var hostname = $scope.slaves[$routeParams.slave_id].hostname;
    var id = pid.substring(0, pid.indexOf('@'));
    var host = hostname + ":" + pid.substring(pid.lastIndexOf(':') + 1);

    $scope.log = function($event) {
      if (!$scope.state.log_dir) {
        $dialog.messageBox(
          'Logging to a file is not enabled',
          "Set the 'log_dir' option if you wish to access the logs.",
          [{label: 'Continue'}]
        ).open();
      } else {
        pailer(host, '/slave/log', 'Mesos Slave');
      }
    };

    var usageRequest = $http.jsonp(
        'http://' + host + '/monitor/usage.json?jsonp=JSON_CALLBACK');

    var stateRequest = $http.jsonp(
        'http://' + host + '/' + id + '/state.json?jsonp=JSON_CALLBACK');

    $q.all([usageRequest, stateRequest]).then(function (responses) {
      $scope.monitor = responses[0].data;
      $scope.state = responses[1].data;

      $scope.slave = {};
      $scope.slave.frameworks = {};
      $scope.slave.completed_frameworks = {};

      $scope.slave.staging_tasks = 0;
      $scope.slave.starting_tasks = 0;
      $scope.slave.running_tasks = 0;

      // Update the framework map.
      _.each($scope.state.frameworks, function(framework) {
        $scope.slave.frameworks[framework.id] = framework;
      });

      // Update the completed framework map.
      _.each($scope.state.completed_frameworks, function(framework) {
        $scope.slave.completed_frameworks[framework.id] = framework;
      });

      // Compute the framework stats.
      _.each(_.values($scope.state.frameworks).concat(_.values($scope.state.completed_frameworks)),
          function(framework) {
            framework.num_tasks = 0;
            framework.cpus = 0;
            framework.mem = 0;

            _.each(framework.executors, function(executor) {
              framework.num_tasks += _.size(executor.tasks);
              framework.cpus += executor.resources.cpus;
              framework.mem += executor.resources.mem;
            });
      });

      $('#slave').show();
    },
    function (reason) {
      $scope.alert_message = 'Failed to get slave usage / state: ' + reason;
      $('#alert').show();
    });
  };

  if ($scope.state) {
    update();
  }

  $(document).on('state_updated', update);
  $scope.$on('$beforeRouteChange', function() {
    $(document).off('state_updated', update);
  });
}


function SlaveFrameworkCtrl($scope, $routeParams, $http, $q) {
  setNavbarActiveTab('slaves');

  $scope.slave_id = $routeParams.slave_id;
  $scope.framework_id = $routeParams.framework_id;

  $scope.tables = {};
  $scope.tables['executors'] = new Table('id');
  $scope.tables['completed_executors'] = new Table('id');

  $scope.columnClass = columnClass($scope);
  $scope.selectColumn = selectColumn($scope);

  var update = function() {
    if (!($routeParams.slave_id in $scope.slaves)) {
      $scope.alert_message = 'No slave found with ID: ' + $routeParams.slave_id;
      $('#alert').show();
      return;
    }

    var pid = $scope.slaves[$routeParams.slave_id].pid;
    var hostname = $scope.slaves[$routeParams.slave_id].hostname;
    var id = pid.substring(0, pid.indexOf('@'));
    var host = hostname + ":" + pid.substring(pid.lastIndexOf(':') + 1)

    var usageRequest = $http.jsonp(
        'http://' + host + '/monitor/usage.json?jsonp=JSON_CALLBACK');

    var stateRequest = $http.jsonp(
        'http://' + host + '/' + id + '/state.json?jsonp=JSON_CALLBACK');

    $q.all([usageRequest, stateRequest]).then(function (responses) {
      var monitor = responses[0].data;
      $scope.state = responses[1].data;

      $scope.slave = {};

      // Find the framework; it's either active or completed.
      $scope.framework = _.find($scope.state.frameworks.concat($scope.state.completed_frameworks),
          function(framework) {
            return $scope.framework_id === framework.id;
          });

      if (!$scope.framework) {
        $scope.alert_message = 'No framework found with ID: ' + $routeParams.framework_id;
        $('#alert').show();
        return;
      }

      // Construct maps of the executors.
      $scope.framework.executors = _.object(
          _.pluck($scope.framework.executors, 'id'), $scope.framework.executors);
      $scope.framework.completed_executors = _.object(
          _.pluck($scope.framework.completed_executors, 'id'), $scope.framework.completed_executors);

      // Compute the framework stats.
      $scope.framework.num_tasks = 0;
      $scope.framework.cpus = 0;
      $scope.framework.mem = 0;

      _.each($scope.framework.executors, function(executor) {
        $scope.framework.num_tasks += _.size(executor.tasks);
        $scope.framework.cpus += executor.resources.cpus;
        $scope.framework.mem += executor.resources.mem;
      });

      // Index the monitoring data.
      $scope.monitor = {};

      $scope.framework.resource_usage = {};
      $scope.framework.resource_usage["cpu_time"] = 0.0;
      $scope.framework.resource_usage["cpu_usage"] = 0.0;
      $scope.framework.resource_usage["memory_rss"] = 0.0;

      _.each(monitor, function(executor) {
        if (!$scope.monitor[executor.framework_id]) {
          $scope.monitor[executor.framework_id] = {};
        }
        $scope.monitor[executor.framework_id][executor.executor_id] = executor;

        $scope.framework.resource_usage["cpu_time"] +=
          executor.resource_usage.cpu_time;
        $scope.framework.resource_usage["cpu_usage"] +=
          executor.resource_usage.cpu_usage;
        $scope.framework.resource_usage["memory_rss"] +=
          executor.resource_usage.memory_rss;
      });

      $('#slave').show();
    },
    function (reason) {
      $scope.alert_message = 'Failed to get slave usage / state: ' + reason;
      $('#alert').show();
    });
  }

  if ($scope.state) {
    update();
  }

  $(document).on('state_updated', update);
  $scope.$on('$beforeRouteChange', function() {
    $(document).off('state_updated', update);
  });
}


function SlaveExecutorCtrl($scope, $routeParams, $http, $q) {
  setNavbarActiveTab('slaves');

  $scope.slave_id = $routeParams.slave_id;
  $scope.framework_id = $routeParams.framework_id;
  $scope.executor_id = $routeParams.executor_id;

  $scope.tables = {};
  $scope.tables['tasks'] = new Table('id');
  $scope.tables['queued_tasks'] = new Table('id');
  $scope.tables['completed_tasks'] = new Table('id');

  $scope.columnClass = columnClass($scope);
  $scope.selectColumn = selectColumn($scope);

  var update = function() {
    if (!($routeParams.slave_id in $scope.slaves)) {
      $scope.alert_message = 'No slave found with ID: ' + $routeParams.slave_id;
      $('#alert').show();
      return;
    }

    var pid = $scope.slaves[$routeParams.slave_id].pid;
    var hostname = $scope.slaves[$routeParams.slave_id].hostname;
    var id = pid.substring(0, pid.indexOf('@'));
    var host = hostname + ":" + pid.substring(pid.lastIndexOf(':') + 1)

    var usageRequest = $http.jsonp(
        'http://' + host + '/monitor/usage.json?jsonp=JSON_CALLBACK');

    var stateRequest = $http.jsonp(
        'http://' + host + '/' + id + '/state.json?jsonp=JSON_CALLBACK');

    $q.all([usageRequest, stateRequest]).then(function (responses) {
      var monitor = responses[0].data;
      $scope.state = responses[1].data;

      $scope.slave = {};

      // Find the framework; it's either active or completed.
      $scope.framework = _.find($scope.state.frameworks.concat($scope.state.completed_frameworks),
          function(framework) {
            return $scope.framework_id === framework.id;
          });

      if (!$scope.framework) {
        $scope.alert_message = 'No framework found with ID: ' + $routeParams.framework_id;
        $('#alert').show();
        return;
      }

      // Look for the executor; it's either active or completed.
      $scope.executor = _.find($scope.framework.executors.concat($scope.framework.completed_executors),
          function(executor) {
            return $scope.executor_id === executor.id;
          });

      if (!$scope.executor) {
        $scope.alert_message = 'No executor found with ID: ' + $routeParams.executor_id;
        $('#alert').show();
        return;
      }

      // Index the monitoring data.
      $scope.monitor = {};

      _.each(monitor, function(executor) {
        if (!$scope.monitor[executor.framework_id]) {
          $scope.monitor[executor.framework_id] = {};
        }
        $scope.monitor[executor.framework_id][executor.executor_id] = executor;
      });

      $('#slave').show();
    },
    function (reason) {
      $scope.alert_message = 'Failed to get slave usage / state: ' + reason;
      $('#alert').show();
    });
  }

  if ($scope.state) {
    update();
  }

  $(document).on('state_updated', update);
  $scope.$on('$beforeRouteChange', function() {
    $(document).off('state_updated', update);
  });
}


function BrowseCtrl($scope, $routeParams, $http) {
  setNavbarActiveTab('slaves');

  var update = function() {
    if ($routeParams.slave_id in $scope.slaves && $routeParams.path) {
      $scope.slave_id = $routeParams.slave_id;
      $scope.path = $routeParams.path;

      var pid = $scope.slaves[$routeParams.slave_id].pid;
      var hostname = $scope.slaves[$routeParams.slave_id].hostname;
      var id = pid.substring(0, pid.indexOf('@'));
      var host = hostname + ":" + pid.substring(pid.lastIndexOf(':') + 1);
      var url = 'http://' + host + '/files/browse.json?jsonp=JSON_CALLBACK';

      $scope.slave_host = host;

      $scope.pail = function($event, path) {
        pailer(host, path, decodeURIComponent(path));
      };

      // TODO(bmahler): Try to get the error code / body in the error callback.
      // This wasn't working with the current version of angular.
      $http.jsonp(url, {params: {path: $routeParams.path}})
        .success(function(data) {
          $scope.listing = data;
          $('#listing').show();
        })
        .error(function() {
          $scope.alert_message = 'Error browsing path: ' + $routeParams.path;
          $('#alert').show();
        });
    } else {
      if (!($routeParams.slave_id in $scope.slaves)) {
        $scope.alert_message = 'No slave found with ID: ' + $routeParams.slave_id;
      } else {
        $scope.alert_message = 'Missing "path" request parameter.';
      }
      $('#alert').show();
    }
  };

  if ($scope.state) {
    update();
  }

  $(document).on('state_updated', update);
  $scope.$on('$beforeRouteChange', function() {
    $(document).off('state_updated', update);
  });
}
