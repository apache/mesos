'use strict';


// Update the outermost scope with the new state.
function update($scope, data) {
  // Don't do anythinf if the data hasn't changed.
  if ($scope.data == data) {
    return;
  }

  $scope.data = data;
  $scope.state = $.parseJSON(data);

  $scope.total_cpus = 0;
  $scope.total_mem = 0;
  $scope.used_cpus = 0;
  $scope.used_mem = 0;
  $scope.offered_cpus = 0;
  $scope.offered_mem = 0;

  $scope.slaves = {};

  _.each($scope.state.slaves, function(slave) {
    $scope.total_cpus += slave.resources.cpus;
    $scope.total_mem += slave.resources.mem;

    $scope.slaves[slave.id] = slave;
  });

  $scope.frameworks = {};
  $scope.offers = {};

  _.each($scope.state.frameworks, function(framework) {
      $scope.used_cpus += framework.resources.cpus;
      $scope.used_mem += framework.resources.mem;

      _.each(framework.offers, function(offer) {
        $scope.offered_cpus = offer.resources.cpus;
        $scope.offered_mem = offer.resources.mem;

        $scope.offers[offer.id] = offer;
      });

      $scope.frameworks[framework.id] = framework;

      if ($scope.total_cpus > 0) {
        $scope.frameworks[framework.id].cpus_share =
          framework.resources.cpus / $scope.total_cpus;
      } else {
        $scope.frameworks[framework.id].cpus_share = 0;
      }

      if ($scope.total_mem > 0) {
        $scope.frameworks[framework.id].mem_share =
          framework.resources.mem / $scope.total_mem;
      } else {
        $scope.frameworks[framework.id].mem_share = 0;
      }

      $scope.frameworks[framework.id].max_share =
        Math.max($scope.frameworks[framework.id].cpus_share,
                 $scope.frameworks[framework.id].mem_share);
  });

  $scope.used_cpus -= $scope.offered_cpus;
  $scope.used_mem -= $scope.offered_mem;

  $scope.idle_cpus = $scope.total_cpus - ($scope.offered_cpus + $scope.used_cpus);
  $scope.idle_mem = $scope.total_mem - ($scope.offered_mem + $scope.used_mem);

  $scope.completed_frameworks = {};

  _.each($scope.state.completed_frameworks, function(framework) {
      $scope.completed_frameworks[framework.id] = framework;
  });

  $.event.trigger('state_updated');
}

// Main controller that can be used to handle "global" events. E.g.,:
//     $scope.$on('$afterRouteChange', function() { ...; });
//
// In addition, the MainCntl encapsulates the "view", allowing the
// active controller/view to easily access anything in scope (e.g.,
// the state).
function MainCntl($scope, $http, $route, $routeParams, $location, $defer) {
  // Turn off the loading gif, turn on the navbar.
  $("#loading").hide();
  $("#navbar").show();

  // Initialize popovers and bind the function used to show a popover.
  Popovers.initialize();
  $scope.popover = Popovers.show;

  $scope.$location = $location;
  $scope.delay = 2000;
  $scope.retry = 0;

  var poll = function() {
    $http.get('master/state.json',
              {transformResponse: function(data) { return data; }})
      .success(function(data) {
        update($scope, data);
        $scope.delay = 2000;
        $defer(poll, $scope.delay);
      })
      .error(function(data) {
        if ($scope.delay >= 128000) {
          $scope.delay = 2000;
        } else {
          $scope.delay = $scope.delay * 2;
        }
        $scope.retry = $scope.delay;
        function countdown() {
          if ($scope.retry == 0) {
            $('#error-modal').modal('hide');
          } else {
            $scope.retry = $scope.retry - 1000;
            $scope.countdown = $defer(countdown, 1000);
          }
        }
        countdown();
        $('#error-modal').modal('show');
      });
  }

  // Make it such that everytime we hide the error-modal, we stop the
  // countdown and restart the polling.
  $('#error-modal').on('hidden', function () {
    if ($scope.countdown != undefined) {
      if ($defer.cancel($scope.countdown)) {
        $scope.delay = 2000; // Restart since they cancelled the countdown.
      }
    }

    // Start polling again, but do it asynchronously (and wait at
    // least a second because otherwise the error-modal won't get
    // properly shown).
    $defer(poll, 1000);
  });

  poll();
}

function HomeCtrl($scope) {
  setNavbarActiveTab('home');
}

var offset = -1;
var log = '';

function LogCtrl($scope, $http, $defer) {
  setNavbarActiveTab('log');

  $('#log').html(log);

  var deferred = undefined;

  var tail = function() {
    $http.get('master/log.json?offset=' + offset)
      .success(function(data) {
        // Get the last "page" of data if this was the first time.
        if (offset == -1) {
          // TODO(benh): Define a "page" size.
          if (data.offset > 1000) {
            offset = data.offset - 1000;
          } else {
            offset = 0;
          }
          deferred = $defer(tail, 0);
          return;
        } else {
          offset = data.offset + data.length;
        }

        if (data.length > 0) {
          // Truncate to the first newline if this is the first time
          // (and we aren't reading from the beginning of the log).
          if (log == '' && data.offset != 0) {
            log = data.data.substring(data.data.indexOf("\n") + 1);
            $('#log').append(log);
          } else {
            log += data.data;
            $('#log').append(data.data);
          }
        }

        deferred = $defer(tail, 1000);
      })
      .error(function(data, status) {
        if (status == 404) {
          $('#log-not-found-alert').show();
        } else {
          deferred = $defer(tail, 1000);
        }
      });
  }

  tail();

  $scope.$on('$beforeRouteChange', function() { $defer.cancel(deferred); });
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
  
}

function FrameworkCtrl($scope, $routeParams) {
  setNavbarActiveTab('frameworks');

  var update = function() {
    if ($routeParams.id in $scope.completed_frameworks) {
      $scope.framework = $scope.completed_frameworks[$routeParams.id];
      $('#terminated-alert').show();
      $('#framework').show();
    } else if ($routeParams.id in $scope.frameworks) {
      $scope.framework = $scope.frameworks[$routeParams.id];
      $('#framework').show();
    } else {
      $('#missing-alert').show();
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
}
