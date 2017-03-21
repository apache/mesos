(function() {
  'use strict';

  var mesosServices = angular.module('mesos.services', []);

  mesosServices.service('$alert', ['$rootScope', function($rootScope) {
    // Types taken from Bootstraps v3's "Alerts"[1] so the type can be used
    // as the class name.
    //
    // [1] http://getbootstrap.com/components/#alerts
    var TYPE_DANGER = 'danger';
    var TYPE_INFO = 'info';
    var TYPE_SUCCESS = 'success';
    var TYPE_WARNING = 'warning';

    var nextId = 1;

    var nextAlerts = [];
    var currentAlerts = $rootScope.currentAlerts = [];

    // Creates an alert to be rendered on the next page view.
    //
    // messageOrOptions - Either a String or an Object that will be used to
    //   render an alert on the next view. If a String, it will be the
    //   message in the alert. If an Object, "title" will be bolded, "message"
    //   will be normal font weight, and "bullets" will be rendered as a list.
    function alert(type, messageOrOptions) {
      var alertObject;

      if (angular.isObject(messageOrOptions)) {
        alertObject = angular.copy(messageOrOptions);
        alertObject.type = type;
      } else {
        alertObject = {
          message: messageOrOptions,
          type: type
        };
      }

      alertObject.id = nextId;
      nextId += 1;
      return nextAlerts.push(alertObject);
    }

    this.danger = function(messageOrOptions) {
      return alert(TYPE_DANGER, messageOrOptions);
    };
    this.info = function(messageOrOptions) {
      return alert(TYPE_INFO, messageOrOptions);
    };
    this.success = function(messageOrOptions) {
      return alert(TYPE_SUCCESS, messageOrOptions);
    };
    this.warning = function(messageOrOptions) {
      return alert(TYPE_WARNING, messageOrOptions);
    };

    // Rotate alerts each time the user navigates.
    $rootScope.$on('$locationChangeSuccess', function() {
      if (nextAlerts.length > 0) {
        // If there are alerts to be shown next, they become the current alerts.
        currentAlerts = $rootScope.currentAlerts = nextAlerts;
        nextAlerts = [];
      } else if (currentAlerts.length > 0) {
        // If there are no next alerts, the current alerts still need to expire
        // if there are any so they won't display again.
        currentAlerts = $rootScope.currentAlerts = [];
      }
    });
  }]);

  var uiModalDialog = angular.module('ui.bootstrap.dialog', ['ui.bootstrap']);
  uiModalDialog
    .factory('$dialog', ['$rootScope', '$modal', function ($rootScope, $modal) {

      var prompt = function(title, message, buttons) {

        if (typeof buttons === 'undefined') {
          buttons = [
            {result:'cancel', label: 'Cancel'},
            {result:'yes', label: 'Yes', cssClass: 'btn-primary'}
          ];
        }

        var ModalCtrl = function($scope, $modalInstance) {
          $scope.title = title;
          $scope.message = message;
          $scope.buttons = buttons;
        };

        return $modal.open({
          templateUrl: 'template/dialog/message.html',
          controller: ModalCtrl
        }).result;
      };

      return {
        prompt: prompt,
        messageBox: function(title, message, buttons) {
          return {
            open: function() {
              return prompt(title, message, buttons);
            }
          };
        }
      };
    }]);

  function Statistics() {
    this.cpus_user_time_secs = 0.0;
    this.cpus_system_time_secs = 0.0;
    this.cpus_limit = 0.0;
    this.cpus_total_usage = 0.0;
    this.mem_rss_bytes = 0.0;
    this.mem_limit_bytes = 0.0;
    this.disk_used_bytes = 0.0;
    this.disk_limit_bytes = 0.0;
    this.timestamp = 0.0;
  }

  Statistics.prototype.add = function(statistics) {
    this.cpus_user_time_secs += statistics.cpus_user_time_secs;
    this.cpus_system_time_secs += statistics.cpus_system_time_secs;
    this.cpus_total_usage += statistics.cpus_total_usage;
    this.cpus_limit += statistics.cpus_limit;
    this.mem_rss_bytes += statistics.mem_rss_bytes;
    this.mem_limit_bytes += statistics.mem_limit_bytes;
    this.disk_used_bytes += statistics.disk_used_bytes;
    this.disk_limit_bytes += statistics.disk_limit_bytes;

    // Set instead of add the timestamp since this is an instantaneous view of
    // CPU usage since the last poll.
    this.timestamp = statistics.timestamp;
  };

  Statistics.prototype.diffUsage = function(statistics) {
    var cpus_user_usage =
      (this.cpus_user_time_secs - statistics.cpus_user_time_secs) /
      (this.timestamp - statistics.timestamp);
    var cpus_system_usage =
      (this.cpus_system_time_secs - statistics.cpus_system_time_secs) /
      (this.timestamp - statistics.timestamp);
    this.cpus_total_usage = cpus_user_usage + cpus_system_usage;
  };

  Statistics.parseJSON = function(json) {
    var statistics = new Statistics();
    statistics.add(json);
    return statistics;
  };

  // Top is an abstraction for polling an agent's monitoring endpoint to
  // periodically update the monitoring data. It also computes CPU usage.
  // This places the following data in scope.monitor:
  //
  //   $scope.monitor = {
  //     "statistics": <stats>,
  //     "frameworks": {
  //       <framework_id>: {
  //         "statistics": <stats>,
  //         "executors": {
  //           <executor_id>: {
  //             "executor_id": <executor_id>,
  //             "framework_id": <framework_id>,
  //             "executor_name: <executor_name>,
  //             "source": <source>,
  //             "statistics": <stats>,
  //           }
  //         }
  //       }
  //     }
  //    }
  //
  // To obtain agent statistics:
  //   $scope.monitor.statistics
  //
  // To obtain a framework's statistics:
  //   $scope.monitor.frameworks[<framework_id>].statistics
  //
  // To obtain an executor's statistics:
  //   $scope.monitor.frameworks[<framework_id>].executors[<executor_id>].statistics
  //
  // In the above,  <stats> is the following object:
  //
  //   {
  //     cpus_user_time_secs: value,
  //     cpus_system_time_secs: value,
  //     cpus_total_usage: value, // Once computed.
  //     mem_limit_bytes: value,
  //     mem_rss_bytes: value,
  //   }
  //
  // TODO(bmahler): The complexity of the monitor object is mostly in place
  // until we have path-params on the monitoring endpoint to request
  // statistics for the agent, or for a specific framework / executor.
  //
  // Arguments:
  //   http: $http service from Angular.
  //   timeout: $timeout service from Angular.
  function Top($http, $timeout) {
    this.http = $http;
    this.timeout = $timeout;
  }

  Top.prototype.poll = function() {
    this.http.jsonp(this.endpoint)

      // Success! Parse the response.
      .success(angular.bind(this, this.parseResponse))

      // Do not continue polling on error.
      .error(angular.noop);
  };

  Top.prototype.parseResponse = function(response) {
    var that = this;
    var monitor = {
      frameworks: {},
      statistics: new Statistics()
    };

    response.forEach(function(executor) {
      var executor_id = executor.executor_id;
      var framework_id = executor.framework_id;
      var current = executor.statistics =
        Statistics.parseJSON(executor.statistics);

      // Compute CPU usage if possible.
      if (that.scope.monitor &&
          that.scope.monitor.frameworks[framework_id] &&
          that.scope.monitor.frameworks[framework_id].executors[executor_id]) {
        var previous = that.scope.monitor.frameworks[framework_id].executors[executor_id].statistics;
        current.diffUsage(previous);
      }

      // Index the data.
      if (!monitor.frameworks[executor.framework_id]) {
        monitor.frameworks[executor.framework_id] = {
          executors: {},
          statistics: new Statistics()
        };
      }

      // Aggregate these statistics into the agent and framework statistics.
      monitor.statistics.add(current);
      monitor.frameworks[executor.framework_id].statistics.add(current);
      monitor.frameworks[executor.framework_id].executors[executor.executor_id] = {
        statistics: current
      };
    });

    if (this.scope.monitor) {
      // Continue polling.
      this.polling = this.timeout(angular.bind(this, this.poll), 3000);
    } else {
      // Try to compute initial CPU usage more quickly than 3 seconds.
      this.polling = this.timeout(angular.bind(this, this.poll), 500);
    }

    // Update the monitoring data.
    this.scope.monitor = monitor;
  };

  // Arguments:
  //   host: host of agent.
  //   id: id of agent.
  //   scope: $scope service from Angular.
  Top.prototype.start = function(host, id, scope) {
    if (this.started()) {
      // TODO(bmahler): Consider logging a warning here.
      return;
    }

    this.endpoint = '//' + host + '/' + id + '/monitor/statistics?jsonp=JSON_CALLBACK';
    this.scope = scope;

    // Initial poll is immediate.
    this.polling = this.timeout(angular.bind(this, this.poll), 0);

    // Stop when we leave the page.
    scope.$on('$routeChangeStart', angular.bind(this, this.stop));
  };

  Top.prototype.started = function() {
    return this.polling != null;
  };

  Top.prototype.stop = function() {
    this.timeout.cancel(this.polling);
    this.polling = null;
  };

  mesosServices.service('top', ['$http', '$timeout', Top]);
})();
