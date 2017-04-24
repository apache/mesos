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
    var url = '//' + host + '/files/read?path=' + path;

    // The randomized `storageKey` is removed from `localStorage` once the
    // pailer window loads the URL into its `sessionStorage`, therefore
    // the probability of collisions is low and we do not use a uuid.
    var storageKey = Math.random().toString(36).substr(2, 8);

    // Store the target URL in `localStorage` which is
    // accessed by the pailer window when opened.
    localStorage.setItem(storageKey, url);

    var pailer =
      window.open('/static/pailer.html', storageKey, 'width=580px, height=700px');

    // Need to use window.onload instead of document.ready to make
    // sure the title doesn't get overwritten.
    pailer.onload = function() {
      pailer.document.title = window_title + ' (' + host + ')';
    };
  }


  function updateInterval(num_agents) {
    // TODO(bmahler): Increasing the update interval for large clusters
    // is done purely to mitigate webui performance issues. Ideally we can
    // keep a consistently fast rate for updating statistical information.
    // For the full system state updates, it may make sense to break
    // it up using pagination and/or splitting the endpoint.
    if (num_agents < 500) {
      return 10000;
    } else if (num_agents < 1000) {
      return 20000;
    } else if (num_agents < 5000) {
      return 60000;
    } else if (num_agents < 10000) {
      return 120000;
    } else if (num_agents < 15000) {
      return 240000;
    } else if (num_agents < 20000) {
      return 480000;
    } else {
      return 960000;
    }
  }

  // Set the task sandbox directory for use by the WebUI.
  function setTaskSandbox(executor) {
    _.each(
        [executor.tasks, executor.queued_tasks, executor.completed_tasks],
        function(tasks) {
      _.each(tasks, function(task) {
        if (executor.type === 'DEFAULT') {
          task.directory = executor.directory + '/tasks/' + task.id;
        } else {
          task.directory = executor.directory;
        };
      });
    });
  }


  // Update the outermost scope with the new state.
  function updateState($scope, $timeout, state) {
    // Don't do anything if the state hasn't changed.
    if ($scope.state == state) {
      return true; // Continue polling.
    }

    $scope.state = state;

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

    // Pass this pollTime to all relativeDate calls to make them all relative to
    // the same moment in time.
    //
    // If relativeDate is called without a reference time, it instantiates a new
    // Date to be the reference. Since there can be hundreds of dates on a given
    // page, they would all be relative to slightly different moments in time.
    $scope.pollTime = new Date();

    // Update the maps.
    $scope.agents = {};
    $scope.frameworks = {};
    $scope.offers = {};
    $scope.completed_frameworks = {};
    $scope.active_tasks = [];
    $scope.unreachable_tasks = [];
    $scope.completed_tasks = [];
    $scope.orphan_tasks = [];

    // Update the stats.
    $scope.cluster = $scope.state.cluster;
    $scope.total_cpus = 0;
    $scope.total_gpus = 0;
    $scope.total_mem = 0;
    $scope.total_disk = 0;
    $scope.used_cpus = 0;
    $scope.used_gpus = 0;
    $scope.used_mem = 0;
    $scope.used_disk = 0;
    $scope.offered_cpus = 0;
    $scope.offered_gpus = 0;
    $scope.offered_mem = 0;
    $scope.offered_disk = 0;

    $scope.activated_agents = $scope.state.activated_slaves;
    $scope.deactivated_agents = $scope.state.deactivated_slaves;
    $scope.unreachable_agents = $scope.state.unreachable_slaves;

    _.each($scope.state.slaves, function(agent) {
      $scope.agents[agent.id] = agent;
      $scope.total_cpus += agent.resources.cpus;
      $scope.total_gpus += agent.resources.gpus;
      $scope.total_mem += agent.resources.mem;
      $scope.total_disk += agent.resources.disk;
    });

    var setTaskMetadata = function(task) {
      if (!task.executor_id) {
        task.executor_id = task.id;
      }
      if (task.statuses.length > 0) {
          var firstStatus = task.statuses[0];
          if (!isStateTerminal(firstStatus.state)) {
              task.start_time = firstStatus.timestamp * 1000;
          }
          var lastStatus = task.statuses[task.statuses.length - 1];
          if (isStateTerminal(task.state)) {
              task.finish_time = lastStatus.timestamp * 1000;
          }
      }
    };

    var isStateTerminal = function(taskState) {
      var terminalStates = [
          'TASK_ERROR',
          'TASK_FAILED',
          'TASK_FINISHED',
          'TASK_KILLED',
          'TASK_LOST',
          'TASK_DROPPED',
          'TASK_GONE',
          'TASK_GONE_BY_OPERATOR'
      ];
      return terminalStates.indexOf(taskState) > -1;
    };

    _.each($scope.state.frameworks, function(framework) {
      $scope.frameworks[framework.id] = framework;

      // Fill in the `roles` field for non-MULTI_ROLE schedulers.
      if (framework.role) {
        framework.roles = [framework.role];
      }

      _.each(framework.offers, function(offer) {
        $scope.offers[offer.id] = offer;
        $scope.offered_cpus += offer.resources.cpus;
        $scope.offered_gpus += offer.resources.gpus;
        $scope.offered_mem += offer.resources.mem;
        $scope.offered_disk += offer.resources.disk;
        offer.framework_name = $scope.frameworks[offer.framework_id].name;
        offer.hostname = $scope.agents[offer.slave_id].hostname;
      });

      $scope.used_cpus += framework.resources.cpus;
      $scope.used_gpus += framework.resources.gpus;
      $scope.used_mem += framework.resources.mem;
      $scope.used_disk += framework.resources.disk;

      framework.cpus_share = 0;
      if ($scope.total_cpus > 0) {
        framework.cpus_share = framework.used_resources.cpus / $scope.total_cpus;
      }

      framework.gpus_share = 0;
      if ($scope.total_gpus > 0) {
        framework.gpus_share = framework.used_resources.gpus / $scope.total_gpus;
      }

      framework.mem_share = 0;
      if ($scope.total_mem > 0) {
        framework.mem_share = framework.used_resources.mem / $scope.total_mem;
      }

      framework.disk_share = 0;
      if ($scope.total_disk > 0) {
        framework.disk_share = framework.used_resources.disk / $scope.total_disk;
      }

      framework.max_share = Math.max(
          framework.cpus_share,
          framework.gpus_share,
          framework.mem_share,
          framework.disk_share);

      // If the executor ID is empty, this is a command executor with an
      // internal executor ID generated from the task ID.
      // TODO(brenden): Remove this once
      // https://issues.apache.org/jira/browse/MESOS-527 is fixed.
      _.each(framework.tasks, setTaskMetadata);
      _.each(framework.unreachable_tasks, setTaskMetadata);
      _.each(framework.completed_tasks, setTaskMetadata);

      $scope.active_tasks = $scope.active_tasks.concat(framework.tasks);
      $scope.unreachable_tasks = $scope.unreachable_tasks.concat(framework.unreachable_tasks);
      $scope.completed_tasks =
        $scope.completed_tasks.concat(framework.completed_tasks);
    });

    _.each($scope.state.completed_frameworks, function(framework) {
      $scope.completed_frameworks[framework.id] = framework;

      // Fill in the `roles` field for non-MULTI_ROLE schedulers.
      if (framework.role) {
        framework.roles = [framework.role];
      }

      _.each(framework.completed_tasks, setTaskMetadata);
    });

    $scope.orphan_tasks = $scope.state.orphan_tasks;
    _.each($scope.orphan_tasks, setTaskMetadata);

    $scope.used_cpus -= $scope.offered_cpus;
    $scope.used_gpus -= $scope.offered_gpus;
    $scope.used_mem -= $scope.offered_mem;
    $scope.used_disk -= $scope.offered_disk;

    $scope.idle_cpus = $scope.total_cpus - ($scope.offered_cpus + $scope.used_cpus);
    $scope.idle_gpus = $scope.total_gpus - ($scope.offered_gpus + $scope.used_gpus);
    $scope.idle_mem = $scope.total_mem - ($scope.offered_mem + $scope.used_mem);
    $scope.idle_disk = $scope.total_disk - ($scope.offered_disk + $scope.used_disk);

    $scope.time_since_update = 0;
    $scope.$broadcast('state_updated');

    return true; // Continue polling.
  }

  // Update the outermost scope with the metrics/snapshot endpoint.
  function updateMetrics($scope, $timeout, metrics) {
    $scope.staging_tasks = metrics['master/tasks_staging'];
    $scope.starting_tasks = metrics['master/tasks_starting'];
    $scope.running_tasks = metrics['master/tasks_running'];
    $scope.killing_tasks = metrics['master/tasks_killing'];
    $scope.finished_tasks = metrics['master/tasks_finished'];
    $scope.killed_tasks = metrics['master/tasks_killed'];
    $scope.failed_tasks = metrics['master/tasks_failed'];
    $scope.lost_tasks = metrics['master/tasks_lost'];

    return true; // Continue polling.
  }


  // Main controller that can be used to handle "global" events. E.g.,:
  //     $scope.$on('$afterRouteChange', function() { ...; });
  //
  // In addition, the MainCtrl encapsulates the "view", allowing the
  // active controller/view to easily access anything in scope (e.g.,
  // the state).
  mesosApp.controller('MainCtrl', [
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
    $scope.isErrorModalOpen = false;

    // Ordered Array of path => activeTab mappings. On successful route changes,
    // the `pathRegexp` values are matched against the current route. The first
    // match will be used to set the active navbar tab.
    var NAVBAR_PATHS = [
      {
        pathRegexp: /^\/agents/,
        tab: 'agents'
      },
      {
        pathRegexp: /^\/frameworks/,
        tab: 'frameworks'
      },
      {
        pathRegexp: /^\/roles/,
        tab: 'roles'
      },
      {
        pathRegexp: /^\/offers/,
        tab: 'offers'
      },
      {
        pathRegexp: /^\/maintenance/,
        tab: 'maintenance'
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

    var leadingMasterURL = function(path) {
      // Use current location as address in case we could not find the
      // leading master.
      var address = location.hostname + ':' + location.port;
      if ($scope.state && $scope.state.leader_info) {
          address = $scope.state.leader_info.hostname + ':' +
                    $scope.state.leader_info.port;
      }

      return '//' + address + path;
    }

    var popupErrorModal = function() {
      if ($scope.delay >= 128000) {
        $scope.delay = 2000;
      } else {
        $scope.delay = $scope.delay * 2;
      }

      $scope.isErrorModalOpen = true;

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
        $scope.isErrorModalOpen = false;

        if ($scope.countdown != null) {
          if ($timeout.cancel($scope.countdown)) {
            // Restart since they cancelled the countdown.
            $scope.delay = 2000;
          }
        }

        // Start polling again, but do it asynchronously (and wait at
        // least a second because otherwise the error-modal won't get
        // properly shown).
        $timeout(pollState, 1000);
        $timeout(pollMetrics, 1000);
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
    };

    var pollState = function() {
      // When the current master is not the leader, the request is redirected to
      // the leading master automatically. This would cause a CORS error if we
      // use XMLHttpRequest here. To avoid the CORS error, we use JSONP as a
      // workaround. Please refer to MESOS-5911 for further details.
      $http.jsonp(leadingMasterURL('/master/state?jsonp=JSON_CALLBACK'))
        .success(function(response) {
          if (updateState($scope, $timeout, response)) {
            $scope.delay = updateInterval(_.size($scope.agents));
            $timeout(pollState, $scope.delay);
          }
        })
        .error(function() {
          if ($scope.isErrorModalOpen === false) {
            popupErrorModal();
          }
        });
    };

    var pollMetrics = function() {
      $http.jsonp(leadingMasterURL('/metrics/snapshot?jsonp=JSON_CALLBACK'))
        .success(function(response) {
          if (updateMetrics($scope, $timeout, response)) {
            $scope.delay = updateInterval(_.size($scope.agents));
            $timeout(pollMetrics, $scope.delay);
          }
        })
        .error(function(message, code) {
          if ($scope.isErrorModalOpen === false) {
            // If return code is 401 or 403 the user is unauthorized to reach
            // the endpoint, which is not a connection error.
            if ([401, 403].indexOf(code) < 0) {
              popupErrorModal();
            }
          }
        });
    };

    pollState();
    pollMetrics();
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

  mesosApp.controller('RolesCtrl', function($scope, $http) {
    var update = function() {
      // TODO(haosdent): Send requests to the leading master directly
      // once `leadingMasterURL` is public.
      $http.jsonp('/master/roles?jsonp=JSON_CALLBACK')
      .success(function(response) {
        $scope.roles = response;
      })
      .error(function() {
        if ($scope.isErrorModalOpen === false) {
          popupErrorModal();
        }
      });
    };

    if ($scope.state) {
      update();
    }

    var removeListener = $scope.$on('state_updated', update);
    $scope.$on('$routeChangeStart', removeListener);
  });

  mesosApp.controller('OffersCtrl', function() {});

  mesosApp.controller('MaintenanceCtrl', function($scope, $http) {
    var update = function() {
      // TODO(haosdent): Send requests to the leading master directly
      // once `leadingMasterURL` is public.
      $http.jsonp('/master/maintenance/schedule?jsonp=JSON_CALLBACK')
      .success(function(response) {
        $scope.maintenance = response;
      })
      .error(function() {
        if ($scope.isErrorModalOpen === false) {
          popupErrorModal();
        }
      });
    };

    if ($scope.state) {
      update();
    }

    var removeListener = $scope.$on('state_updated', update);
    $scope.$on('$routeChangeStart', removeListener);
  });

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


  mesosApp.controller('AgentsCtrl', function() {});


  mesosApp.controller('AgentCtrl', [
      '$dialog', '$scope', '$routeParams', '$http', '$q', '$timeout', 'top',
      function($dialog, $scope, $routeParams, $http, $q, $timeout, $top) {
    $scope.agent_id = $routeParams.agent_id;

    var update = function() {
      if (!($routeParams.agent_id in $scope.agents)) {
        $scope.alert_message = 'No agent found with ID: ' + $routeParams.agent_id;
        $('#alert').show();
        return;
      }

      var pid = $scope.agents[$routeParams.agent_id].pid;
      var hostname = $scope.agents[$routeParams.agent_id].hostname;
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
          pailer(host, '/slave/log', 'Mesos Agent');
        }
      };

      // Set up polling for the monitor if this is the first update.
      if (!$top.started()) {
        $top.start(host, id, $scope);
      }

      $http.jsonp('//' + host + '/' + id + '/state?jsonp=JSON_CALLBACK')
        .success(function (response) {
          $scope.state = response;

          $scope.agent = {};
          $scope.agent.frameworks = {};
          $scope.agent.completed_frameworks = {};

          // Computes framework stats by setting new attributes on the 'framework'
          // object.
          function computeFrameworkStats(framework) {
            framework.num_tasks = 0;
            framework.cpus = 0;
            framework.gpus = 0;
            framework.mem = 0;
            framework.disk = 0;

            _.each(framework.executors, function(executor) {
              framework.num_tasks += _.size(executor.tasks);
              framework.cpus += executor.resources.cpus;
              framework.gpus += executor.resources.gpus;
              framework.mem += executor.resources.mem;
              framework.disk += executor.resources.disk;
            });
          }

          // Compute framework stats and update agent's mappings of those
          // frameworks.
          _.each($scope.state.frameworks, function(framework) {
            $scope.agent.frameworks[framework.id] = framework;
            computeFrameworkStats(framework);

            // Fill in the `roles` field for non-MULTI_ROLE schedulers.
            if (framework.role) {
              framework.roles = [framework.role];
            }
          });

          _.each($scope.state.completed_frameworks, function(framework) {
            $scope.agent.completed_frameworks[framework.id] = framework;
            computeFrameworkStats(framework);

            // Fill in the `roles` field for non-MULTI_ROLE schedulers.
            if (framework.role) {
              framework.roles = [framework.role];
            }
          });

          $scope.state.allocated_resources = {};
          $scope.state.allocated_resources.cpus = 0;
          $scope.state.allocated_resources.gpus = 0;
          $scope.state.allocated_resources.mem = 0;
          $scope.state.allocated_resources.disk = 0;

          // Currently the agent does not expose the total allocated
          // resources across all frameworks, so we sum manually.
          _.each($scope.state.frameworks, function(framework) {
            $scope.state.allocated_resources.cpus += framework.cpus;
            $scope.state.allocated_resources.gpus += framework.gpus;
            $scope.state.allocated_resources.mem += framework.mem;
            $scope.state.allocated_resources.disk += framework.disk;
          });

          $('#agent').show();
        })
        .error(function(reason) {
          $scope.alert_message = 'Failed to get agent usage / state: ' + reason;
          $('#alert').show();
        });

      $http.jsonp('//' + host + '/metrics/snapshot?jsonp=JSON_CALLBACK')
        .success(function (response) {
          $scope.staging_tasks = response['slave/tasks_staging'];
          $scope.starting_tasks = response['slave/tasks_starting'];
          $scope.running_tasks = response['slave/tasks_running'];
          $scope.killing_tasks = response['slave/tasks_killing'];
          $scope.finished_tasks = response['slave/tasks_finished'];
          $scope.killed_tasks = response['slave/tasks_killed'];
          $scope.failed_tasks = response['slave/tasks_failed'];
          $scope.lost_tasks = response['slave/tasks_lost'];
        })
        .error(function(reason) {
          $scope.alert_message = 'Failed to get agent metrics: ' + reason;
          $('#alert').show();
        });
    };

    if ($scope.state) {
      update();
    }

    var removeListener = $scope.$on('state_updated', update);
    $scope.$on('$routeChangeStart', removeListener);
  }]);


  mesosApp.controller('AgentFrameworkCtrl', [
      '$scope', '$routeParams', '$http', '$q', '$timeout', 'top',
      function($scope, $routeParams, $http, $q, $timeout, $top) {
    $scope.agent_id = $routeParams.agent_id;
    $scope.framework_id = $routeParams.framework_id;

    var update = function() {
      if (!($routeParams.agent_id in $scope.agents)) {
        $scope.alert_message = 'No agent found with ID: ' + $routeParams.agent_id;
        $('#alert').show();
        return;
      }

      var pid = $scope.agents[$routeParams.agent_id].pid;
      var hostname = $scope.agents[$routeParams.agent_id].hostname;
      var id = pid.substring(0, pid.indexOf('@'));
      var host = hostname + ":" + pid.substring(pid.lastIndexOf(':') + 1);

      // Set up polling for the monitor if this is the first update.
      if (!$top.started()) {
        $top.start(host, id, $scope);
      }

      $http.jsonp('//' + host + '/' + id + '/state?jsonp=JSON_CALLBACK')
        .success(function (response) {
          $scope.state = response;

          $scope.agent = {};

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

          // Fill in the `roles` field for non-MULTI_ROLE schedulers.
          if ($scope.framework.role) {
            $scope.framework.roles = [$scope.framework.role];
          }

          // Compute the framework stats.
          $scope.framework.num_tasks = 0;
          $scope.framework.cpus = 0;
          $scope.framework.gpus = 0;
          $scope.framework.mem = 0;
          $scope.framework.disk = 0;

          _.each($scope.framework.executors, function(executor) {
            $scope.framework.num_tasks += _.size(executor.tasks);
            $scope.framework.cpus += executor.resources.cpus;
            $scope.framework.gpus += executor.resources.gpus;
            $scope.framework.mem += executor.resources.mem;
            $scope.framework.disk += executor.resources.disk;

            // If 'role' is not present in executor, we are talking
            // to a non-MULTI_ROLE capable agent. This means that we
            // can use the 'role' of the framework.
            executor.role = executor.role || $scope.framework.role;
          });

          $('#agent').show();
        })
        .error(function (reason) {
          $scope.alert_message = 'Failed to get agent usage / state: ' + reason;
          $('#alert').show();
        });
    };

    if ($scope.state) {
      update();
    }

    var removeListener = $scope.$on('state_updated', update);
    $scope.$on('$routeChangeStart', removeListener);
  }]);


  mesosApp.controller('AgentExecutorCtrl', [
      '$scope', '$routeParams', '$http', '$q', '$timeout', 'top',
      function($scope, $routeParams, $http, $q, $timeout, $top) {
    $scope.agent_id = $routeParams.agent_id;
    $scope.framework_id = $routeParams.framework_id;
    $scope.executor_id = $routeParams.executor_id;

    var update = function() {
      if (!($routeParams.agent_id in $scope.agents)) {
        $scope.alert_message = 'No agent found with ID: ' + $routeParams.agent_id;
        $('#alert').show();
        return;
      }

      var pid = $scope.agents[$routeParams.agent_id].pid;
      var hostname = $scope.agents[$routeParams.agent_id].hostname;
      var id = pid.substring(0, pid.indexOf('@'));
      var host = hostname + ":" + pid.substring(pid.lastIndexOf(':') + 1);

      // Set up polling for the monitor if this is the first update.
      if (!$top.started()) {
        $top.start(host, id, $scope);
      }

      $http.jsonp('//' + host + '/' + id + '/state?jsonp=JSON_CALLBACK')
        .success(function (response) {
          $scope.state = response;

          $scope.agent = {};

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

          // If 'role' is not present in the task, we are talking
          // to a non-MULTI_ROLE capable agent. This means that we
          // can use the 'role' of the framework.
          if (!("role" in $scope.executor)) {
            $scope.executor.role = $scope.framework.role;

            function setRole(tasks) {
              _.each(tasks, function(task) {
                task.role = $scope.framework.role;
              });
            }

            setRole($scope.executor.tasks);
            setRole($scope.executor.queued_tasks);
            setRole($scope.executor.completed_tasks);
          }

          setTaskSandbox($scope.executor);

          $('#agent').show();
        })
        .error(function (reason) {
          $scope.alert_message = 'Failed to get agent usage / state: ' + reason;
          $('#alert').show();
        });
    };

    if ($scope.state) {
      update();
    }

    var removeListener = $scope.$on('state_updated', update);
    $scope.$on('$routeChangeStart', removeListener);
  }]);


  // Reroutes requests like:
  //   * '/agents/:agent_id/frameworks/:framework_id/executors/:executor_id/browse'
  //   * '/agents/:agent_id/frameworks/:framework_id/executors/:executor_id/tasks/:task_id/browse'
  // to the sandbox directory of the executor or the task respectively. This
  // requires a second request because the directory to browse is known by the
  // agent but not by the master. Request the directory from the agent, and then
  // redirect to it.
  //
  // TODO(ssorallen): Add `executor.directory` to the master's state endpoint
  // output so this controller of rerouting is no longer necessary.
  mesosApp.controller('AgentTaskAndExecutorRerouterCtrl',
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
    if (!$scope.agents) {
      $alert.danger({
        message: "Navigate to the agent's sandbox via the Mesos UI.",
        title: "Failed to find agents."
      });
      return $location.path('/').replace();
    }

    var agent = $scope.agents[$routeParams.agent_id];

    // If the agent doesn't exist, send the user back.
    if (!agent) {
      return goBack("Agent with ID '" + $routeParams.agent_id + "' does not exist.");
    }

    var pid = agent.pid;
    var hostname = $scope.agents[$routeParams.agent_id].hostname;
    var id = pid.substring(0, pid.indexOf('@'));
    var port = pid.substring(pid.lastIndexOf(':') + 1);
    var host = hostname + ":" + port;

    // Request agent details to get access to the route executor's "directory"
    // to navigate directly to the executor's sandbox.
    $http.jsonp('//' + host + '/' + id + '/state?jsonp=JSON_CALLBACK')
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
              "' does not exist on agent with ID '" + $routeParams.agent_id +
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
              "' does not exist on agent with ID '" + $routeParams.agent_id +
              "'."
          );
        }

        var sandboxDirectory = executor.directory;

        function matchTask(task) {
          return $routeParams.task_id === task.id;
        }

        // Continue to navigate to the task's sandbox if the task id is
        // specified in route parameters.
        if ($routeParams.task_id) {
          setTaskSandbox(executor);

          var task =
            _.find(executor.tasks, matchTask) ||
            _.find(executor.queued_tasks, matchTask) ||
            _.find(executor.completed_tasks, matchTask);

          if (!task) {
            return goBack(
              "Task with ID '" + $routeParams.task_id +
                "' does not exist on agent with ID '" + $routeParams.agent_id +
                "'."
            );
          }

          sandboxDirectory = task.directory;
        }

        // Navigate to a path like '/agents/:id/browse?path=%2Ftmp%2F', the
        // recognized "browse" endpoint for an agent.
        $location.path('/agents/' + $routeParams.agent_id + '/browse')
          .search({path: sandboxDirectory})
          .replace();
      })
      .error(function(response) {
        $alert.danger({
          bullets: [
            "The agent's hostname, '" + hostname + "', is not accessible from your network",
            "The agent's port, '" + port + "', is not accessible from your network",
            "The agent timed out or went offline"
          ],
          message: "Potential reasons:",
          title: "Failed to connect to agent '" + $routeParams.agent_id +
            "' on '" + host + "'."
        });

        // Is the agent dead? Navigate home since returning to the agent might
        // end up in an endless loop.
        $location.path('/').replace();
      });
  });


  mesosApp.controller('BrowseCtrl', function($scope, $routeParams, $http) {
    var update = function() {
      if ($routeParams.agent_id in $scope.agents && $routeParams.path) {
        $scope.agent_id = $routeParams.agent_id;
        $scope.path = $routeParams.path;

        var pid = $scope.agents[$routeParams.agent_id].pid;
        var hostname = $scope.agents[$routeParams.agent_id].hostname;
        var id = pid.substring(0, pid.indexOf('@'));
        var host = hostname + ":" + pid.substring(pid.lastIndexOf(':') + 1);
        var url = '//' + host + '/files/browse?jsonp=JSON_CALLBACK';

        $scope.agent_host = host;

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
        if (!($routeParams.agent_id in $scope.agents)) {
          $scope.alert_message = 'No agent found with ID: ' + $routeParams.agent_id;
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
