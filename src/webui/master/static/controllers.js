'use strict';

function hasSelectedText () {
  if (window.getSelection) {  // All browsers except IE before version 9.
    var range = window.getSelection();
    return range.toString().length > 0;
  }
  return false;
}


// Update the outermost scope with the new state.
function update($scope, $defer, data) {
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
          $defer(countdown, 1000);
        }
      }
      countdown();
      return false; // Don't continue polling.
    }
  }

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
  $scope.total_cpus = 0;
  $scope.total_mem = 0;
  $scope.used_cpus = 0;
  $scope.used_mem = 0;
  $scope.offered_cpus = 0;
  $scope.offered_mem = 0;

  _.each($scope.slaves, function(slave) {
    $scope.total_cpus += slave.resources.cpus;
    $scope.total_mem += slave.resources.mem;
  });

  _.each($scope.frameworks, function(framework) {
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
  });

  _.each($scope.offers, function(offer) {
    $scope.offered_cpus += offer.resources.cpus;
    $scope.offered_mem += offer.resources.mem;
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
  $scope.time_since_update = 0;

  var poll = function() {
    $http.get('master/state.json',
              {transformResponse: function(data) { return data; }})
      .success(function(data) {
        if (update($scope, $defer, data)) {
          $scope.delay = 2000;
          $defer(poll, $scope.delay);
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

  $scope.log = function($event) {
    if (!$scope.state.log_dir) {
      $('#no-log-dir-modal').modal('show');
    } else {
      var url = '/files/read.json?name=/log';
      var pailer =
        window.open('/static/pailer.html', url, 'width=580px, height=700px');

      // Need to use window.onload instead of document.ready to make
      // sure the title doesn't get overwritten.
      pailer.onload = function() {
        pailer.document.title = 'Mesos Master (' + location.host + ')';
      }
    }
  }
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


function SlaveCtrl($scope, $routeParams, $http) {
  setNavbarActiveTab('slaves');

  var host = undefined;

  $scope.log = function($event) {
    if (!$scope.state.log_dir) {
      $('#no-log-dir-modal').modal('show');
    } else {
      var url = 'http://' + host + '/files/read.json?name=/log';
      var pailer =
        window.open('/static/pailer.html', url, 'width=580px, height=700px');

     // Need to use window.onload instead of document.ready to make
      // sure the title doesn't get overwritten.
      pailer.onload = function() {
        pailer.document.title = 'Mesos Slave (' + host + ')';
      }
    }
  }

  var update = function() {
    if ($routeParams.id in $scope.slaves) {
      var pid = $scope.slaves[$routeParams.id].pid;
      var id = pid.substring(0, pid.indexOf('@'));
      host = pid.substring(pid.indexOf('@') + 1);
      var url = 'http://' + host + '/' + id
        + '/state.json?jsonp=JSON_CALLBACK';
      $http.jsonp(url)
        .success(function(data) {
          $scope.state = data;
          $('#slave').show();
        })
        .error(function() {
          alert('unimplemented');
        });
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
