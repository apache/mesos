(function() {
  'use strict';

  var mesosApp = angular.module('mesos');

  function hasSelectedText() {
    if (window.getSelection) {  // All browsers except IE before version 9.
      var range = window.getSelection();
      return range.toString().length > 0;
    }
    return false;
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
    };
  }


  function updateInterval(num_slaves) {
    // TODO(bmahler): Increasing the update interval for large clusters
    // is done purely to mitigate webui performance issues. Ideally we can
    // keep a consistently fast rate for updating statistical information.
    // For the full system state updates, it may make sense to break
    // it up using pagination and/or splitting the endpoint.
    if (num_slaves < 500) {
      return 10000;
    } else if (num_slaves < 1000) {
      return 20000;
    } else if (num_slaves < 5000) {
      return 60000;
    } else if (num_slaves < 10000) {
      return 120000;
    } else if (num_slaves < 15000) {
      return 240000;
    } else if (num_slaves < 20000) {
      return 480000;
    } else {
      return 960000;
    }
  }


  // Update the outermost scope with the new state.
  function update($scope, $timeout, data) {
    // Don't do anything if the data hasn't changed.
    if ($scope.data == data) {
      return true; // Continue polling.
    }

    $scope.state = JSON.parse(data);

    // Determine if there is a leader (and redirect if not the leader).
    if ($scope.state.leader) {

      // Redirect if we aren't the leader.
      if ($scope.state.leader != $scope.state.pid) {
        $scope.redirect = 6000;
        $("#not-leader-alert").removeClass("hide");

        var countdown = function() {
          if ($scope.redirect == 0) {
            // TODO(benh): Use '$window'.
            window.location = '/master/redirect';
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

    // Pass this pollTime to all relativeDate calls to make them all relative to
    // the same moment in time.
    //
    // If relativeDate is called without a reference time, it instantiates a new
    // Date to be the reference. Since there can be hundreds of dates on a given
    // page, they would all be relative to slightly different moments in time.
    $scope.pollTime = new Date();

    // Update the maps.
    $scope.slaves = {};
    $scope.frameworks = {};
    $scope.offers = {};
    $scope.completed_frameworks = {};
    $scope.active_tasks = [];
    $scope.completed_tasks = [];

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

    _.each($scope.state.slaves, function(slave) {
      $scope.slaves[slave.id] = slave;
      $scope.total_cpus += slave.resources.cpus;
      $scope.total_mem += slave.resources.mem;
    });

    var setTaskMetadata = function(task) {
      if (!task.executor_id) {
        task.executor_id = task.id;
      }
      if (task.statuses.length > 0) {
        task.start_time = task.statuses[0].timestamp * 1000;
        task.finish_time =
          task.statuses[task.statuses.length - 1].timestamp * 1000;
      }
    };

    _.each($scope.state.frameworks, function(framework) {
      $scope.frameworks[framework.id] = framework;

      _.each(framework.offers, function(offer) {
        $scope.offers[offer.id] = offer;
        $scope.offered_cpus += offer.resources.cpus;
        $scope.offered_mem += offer.resources.mem;
        offer.framework_name = $scope.frameworks[offer.framework_id].name;
        offer.hostname = $scope.slaves[offer.slave_id].hostname;
      });

      $scope.used_cpus += framework.resources.cpus;
      $scope.used_mem += framework.resources.mem;

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
      _.each(framework.tasks, setTaskMetadata);
      _.each(framework.completed_tasks, setTaskMetadata);

      $scope.active_tasks = $scope.active_tasks.concat(framework.tasks);
      $scope.completed_tasks =
        $scope.completed_tasks.concat(framework.completed_tasks);
    });

    _.each($scope.state.completed_frameworks, function(framework) {
      $scope.completed_frameworks[framework.id] = framework;

      _.each(framework.completed_tasks, setTaskMetadata);
    });

    $scope.used_cpus -= $scope.offered_cpus;
    $scope.used_mem -= $scope.offered_mem;

    $scope.idle_cpus = $scope.total_cpus - ($scope.offered_cpus + $scope.used_cpus);
    $scope.idle_mem = $scope.total_mem - ($scope.offered_mem + $scope.used_mem);

    $scope.time_since_update = 0;
    $scope.$broadcast('state_updated');

    return true; // Continue polling.
  }


  // Main controller that can be used to handle "global" events. E.g.,:
  //     $scope.$on('$afterRouteChange', function() { ...; });
  //
  // In addition, the MainCntl encapsulates the "view", allowing the
  // active controller/view to easily access anything in scope (e.g.,
  // the state).
  mesosApp.controller('MainCntl', [
      '$scope', '$http', '$location', '$timeout', '$modal',
      function($scope, $http, $location, $timeout, $modal) {
    $scope.doneLoading = true;

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

    // Ordered Array of path => activeTab mappings. On successful route changes,
    // the `pathRegexp` values are matched against the current route. The first
    // match will be used to set the active navbar tab.
    var NAVBAR_PATHS = [
      {
        pathRegexp: /^\/slaves/,
        tab: 'slaves'
      },
      {
        pathRegexp: /^\/frameworks/,
        tab: 'frameworks'
      },
      {
        pathRegexp: /^\/offers/,
        tab: 'offers'
      }
    ];

    // Set the active tab on route changes according to NAVBAR_PATHS.
    $scope.$on('$routeChangeSuccess', function(event, current) {
      var path = current.$$route.originalPath;

      // Use _.some so the loop can exit on the first `pathRegexp` match.
      var matched = _.some(NAVBAR_PATHS, function(nav) {
        if (path.match(nav.pathRegexp)) {
          $scope.navbarActiveTab = nav.tab;
          return true;
        }
      });

      if (!matched) $scope.navbarActiveTab = null;
    });

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

          var errorModal = $modal.open({
            controller: function($scope, $modalInstance, scope) {
              // Give the modal reference to the root scope so it can access the
              // `retry` variable. It needs to be passed by reference, not by
              // value, since its value is changed outside the scope of the
              // modal.
              $scope.rootScope = scope;
            },
            resolve: {
              scope: function() { return $scope; }
            },
            templateUrl: "template/dialog/masterGone.html"
          });

          // Make it such that everytime we hide the error-modal, we stop the
          // countdown and restart the polling.
          errorModal.result.then(function() {
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
          });

          $scope.retry = $scope.delay;
          var countdown = function() {
            if ($scope.retry === 0) {
              errorModal.close();
            } else {
              $scope.retry = $scope.retry - 1000;
              $scope.countdown = $timeout(countdown, 1000);
            }
          };
          countdown();
        });
    };

    poll();
  }]);


  mesosApp.controller('HomeCtrl', function($dialog, $scope) {
    $scope.log = function($event) {
      if (!$scope.state.external_log_file && !$scope.state.log_dir) {
        $dialog.messageBox(
          'Logging to a file is not enabled',
          "Set the 'external_log_file' or 'log_dir' option if you wish to access the logs.",
          [{label: 'Continue'}]
        ).open();
      } else {
        pailer(
            $scope.$location.host() + ':' + $scope.$location.port(),
            '/master/log',
            'Mesos Master');
      }
    };
  });

  mesosApp.controller('FrameworksCtrl', function() {});

  mesosApp.controller('OffersCtrl', function() {});

  mesosApp.controller('FrameworkCtrl', function($scope, $routeParams) {
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
    };

    if ($scope.state) {
      update();
    }

    var removeListener = $scope.$on('state_updated', update);
    $scope.$on('$routeChangeStart', removeListener);
  });


  mesosApp.controller('SlavesCtrl', function() {});


  mesosApp.controller('SlaveCtrl', [
      '$dialog', '$scope', '$routeParams', '$http', '$q', '$timeout', 'top',
      function($dialog, $scope, $routeParams, $http, $q, $timeout, $top) {
    $scope.slave_id = $routeParams.slave_id;

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
        if (!$scope.state.external_log_file && !$scope.state.log_dir) {
          $dialog.messageBox(
            'Logging to a file is not enabled',
            "Set the 'external_log_file' or 'log_dir' option if you wish to access the logs.",
            [{label: 'Continue'}]
          ).open();
        } else {
          pailer(host, '/slave/log', 'Mesos Slave');
        }
      };

      // Set up polling for the monitor if this is the first update.
      if (!$top.started()) {
        $top.start(host, $scope);
      }

      $http.jsonp('http://' + host + '/' + id + '/state.json?jsonp=JSON_CALLBACK')
        .success(function (response) {
          $scope.state = response;

          $scope.slave = {};
          $scope.slave.frameworks = {};
          $scope.slave.completed_frameworks = {};

          $scope.slave.staging_tasks = 0;
          $scope.slave.starting_tasks = 0;
          $scope.slave.running_tasks = 0;

          // Computes framework stats by setting new attributes on the 'framework'
          // object.
          function computeFrameworkStats(framework) {
            framework.num_tasks = 0;
            framework.cpus = 0;
            framework.mem = 0;

            _.each(framework.executors, function(executor) {
              framework.num_tasks += _.size(executor.tasks);
              framework.cpus += executor.resources.cpus;
              framework.mem += executor.resources.mem;
            });
          }

          // Compute framework stats and update slave's mappings of those
          // frameworks.
          _.each($scope.state.frameworks, function(framework) {
            $scope.slave.frameworks[framework.id] = framework;
            computeFrameworkStats(framework);
          });

          _.each($scope.state.completed_frameworks, function(framework) {
            $scope.slave.completed_frameworks[framework.id] = framework;
            computeFrameworkStats(framework);
          });

          $('#slave').show();
        })
        .error(function(reason) {
          $scope.alert_message = 'Failed to get slave usage / state: ' + reason;
          $('#alert').show();
        });
    };

    if ($scope.state) {
      update();
    }

    var removeListener = $scope.$on('state_updated', update);
    $scope.$on('$routeChangeStart', removeListener);
  }]);


  mesosApp.controller('SlaveFrameworkCtrl', [
      '$scope', '$routeParams', '$http', '$q', '$timeout', 'top',
      function($scope, $routeParams, $http, $q, $timeout, $top) {
    $scope.slave_id = $routeParams.slave_id;
    $scope.framework_id = $routeParams.framework_id;

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

      // Set up polling for the monitor if this is the first update.
      if (!$top.started()) {
        $top.start(host, $scope);
      }

      $http.jsonp('http://' + host + '/' + id + '/state.json?jsonp=JSON_CALLBACK')
        .success(function (response) {
          $scope.state = response;

          $scope.slave = {};

          function matchFramework(framework) {
            return $scope.framework_id === framework.id;
          }

          // Find the framework; it's either active or completed.
          $scope.framework =
              _.find($scope.state.frameworks, matchFramework) ||
              _.find($scope.state.completed_frameworks, matchFramework);

          if (!$scope.framework) {
            $scope.alert_message = 'No framework found with ID: ' + $routeParams.framework_id;
            $('#alert').show();
            return;
          }

          // Compute the framework stats.
          $scope.framework.num_tasks = 0;
          $scope.framework.cpus = 0;
          $scope.framework.mem = 0;

          _.each($scope.framework.executors, function(executor) {
            $scope.framework.num_tasks += _.size(executor.tasks);
            $scope.framework.cpus += executor.resources.cpus;
            $scope.framework.mem += executor.resources.mem;
          });

          $('#slave').show();
        })
        .error(function (reason) {
          $scope.alert_message = 'Failed to get slave usage / state: ' + reason;
          $('#alert').show();
        });
    };

    if ($scope.state) {
      update();
    }

    var removeListener = $scope.$on('state_updated', update);
    $scope.$on('$routeChangeStart', removeListener);
  }]);


  mesosApp.controller('SlaveExecutorCtrl', [
      '$scope', '$routeParams', '$http', '$q', '$timeout', 'top',
      function($scope, $routeParams, $http, $q, $timeout, $top) {
    $scope.slave_id = $routeParams.slave_id;
    $scope.framework_id = $routeParams.framework_id;
    $scope.executor_id = $routeParams.executor_id;

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

      // Set up polling for the monitor if this is the first update.
      if (!$top.started()) {
        $top.start(host, $scope);
      }

      $http.jsonp('http://' + host + '/' + id + '/state.json?jsonp=JSON_CALLBACK')
        .success(function (response) {
          $scope.state = response;

          $scope.slave = {};

          function matchFramework(framework) {
            return $scope.framework_id === framework.id;
          }

          // Find the framework; it's either active or completed.
          $scope.framework =
            _.find($scope.state.frameworks, matchFramework) ||
            _.find($scope.state.completed_frameworks, matchFramework);

          if (!$scope.framework) {
            $scope.alert_message = 'No framework found with ID: ' + $routeParams.framework_id;
            $('#alert').show();
            return;
          }

          function matchExecutor(executor) {
            return $scope.executor_id === executor.id;
          }

          // Look for the executor; it's either active or completed.
          $scope.executor =
            _.find($scope.framework.executors, matchExecutor) ||
            _.find($scope.framework.completed_executors, matchExecutor);

          if (!$scope.executor) {
            $scope.alert_message = 'No executor found with ID: ' + $routeParams.executor_id;
            $('#alert').show();
            return;
          }

          $('#slave').show();
        })
        .error(function (reason) {
          $scope.alert_message = 'Failed to get slave usage / state: ' + reason;
          $('#alert').show();
        });
    };

    if ($scope.state) {
      update();
    }

    var removeListener = $scope.$on('state_updated', update);
    $scope.$on('$routeChangeStart', removeListener);
  }]);


  // Reroutes a request like
  // '/slaves/:slave_id/frameworks/:framework_id/executors/:executor_id/browse'
  // to the executor's sandbox. This requires a second request because the
  // directory to browse is known by the slave but not by the master. Request
  // the directory from the slave, and then redirect to it.
  //
  // TODO(ssorallen): Add `executor.directory` to the state.json output so this
  // controller of rerouting is no longer necessary.
  mesosApp.controller('SlaveExecutorRerouterCtrl',
      function($alert, $http, $location, $routeParams, $scope, $window) {

    function goBack(flashMessageOrOptions) {
      if (flashMessageOrOptions) {
        $alert.danger(flashMessageOrOptions);
      }

      if ($window.history.length > 1) {
        // If the browser has something in its history, just go back.
        $window.history.back();
      } else {
        // Otherwise navigate to the framework page, which is likely the
        // previous page anyway.
        $location.path('/frameworks/' + $routeParams.framework_id).replace();
      }
    }

    // When navigating directly to this page, e.g. pasting the URL into the
    // browser, the previous page is not a page in Mesos. In that case, navigate
    // home.
    if (!$scope.slaves) {
      $alert.danger({
        message: "Navigate to the slave's sandbox via the Mesos UI.",
        title: "Failed to find slaves."
      });
      return $location.path('/').replace();
    }

    var slave = $scope.slaves[$routeParams.slave_id];

    // If the slave doesn't exist, send the user back.
    if (!slave) {
      return goBack("Slave with ID '" + $routeParams.slave_id + "' does not exist.");
    }

    var pid = slave.pid;
    var hostname = $scope.slaves[$routeParams.slave_id].hostname;
    var id = pid.substring(0, pid.indexOf('@'));
    var port = pid.substring(pid.lastIndexOf(':') + 1);
    var host = hostname + ":" + port;

    // Request slave details to get access to the route executor's "directory"
    // to navigate directly to the executor's sandbox.
    $http.jsonp('http://' + host + '/' + id + '/state.json?jsonp=JSON_CALLBACK')
      .success(function(response) {

        function matchFramework(framework) {
          return $routeParams.framework_id === framework.id;
        }

        var framework =
          _.find(response.frameworks, matchFramework) ||
          _.find(response.completed_frameworks, matchFramework);

        if (!framework) {
          return goBack(
            "Framework with ID '" + $routeParams.framework_id +
              "' does not exist on slave with ID '" + $routeParams.slave_id +
              "'."
          );
        }

        function matchExecutor(executor) {
          return $routeParams.executor_id === executor.id;
        }

        var executor =
          _.find(framework.executors, matchExecutor) ||
          _.find(framework.completed_executors, matchExecutor);

        if (!executor) {
          return goBack(
            "Executor with ID '" + $routeParams.executor_id +
              "' does not exist on slave with ID '" + $routeParams.slave_id +
              "'."
          );
        }

        // Navigate to a path like '/slaves/:id/browse?path=%2Ftmp%2F', the
        // recognized "browse" endpoint for a slave.
        $location.path('/slaves/' + $routeParams.slave_id + '/browse')
          .search({path: executor.directory})
          .replace();
      })
      .error(function(response) {
        $alert.danger({
          bullets: [
            "The slave's hostname, '" + hostname + "', is not accessible from your network",
            "The slave's port, '" + port + "', is not accessible from your network",
            "The slave timed out or went offline"
          ],
          message: "Potential reasons:",
          title: "Failed to connect to slave '" + $routeParams.slave_id +
            "' on '" + host + "'."
        });

        // Is the slave dead? Navigate home since returning to the slave might
        // end up in an endless loop.
        $location.path('/').replace();
      });
  });


  mesosApp.controller('BrowseCtrl', function($scope, $routeParams, $http) {
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

    var removeListener = $scope.$on('state_updated', update);
    $scope.$on('$routeChangeStart', removeListener);
  });
})();
