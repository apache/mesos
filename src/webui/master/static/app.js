'use strict';

// TODO(bmahler): Upgrade AngularJS past 1.0.0 in order to use the full
// angular-ui.js suite of directives / filters. (Punting as there are breaking
// changes to take care of).
angular.module('mesos', []).
  config(['$routeProvider', function($routeProvider) {
    $routeProvider
      .when('/', {template: 'static/home.html', controller: HomeCtrl})
      .when('/dashboard', {template: 'static/dashboard.html', controller: DashboardCtrl})
      .when('/frameworks', {template: 'static/frameworks.html', controller: FrameworksCtrl})
      .when('/frameworks/:id', {template: 'static/framework.html', controller: FrameworkCtrl})
      .when('/slaves', {template: 'static/slaves.html', controller: SlavesCtrl})
      .when('/slaves/:slave_id', {template: 'static/slave.html', controller: SlaveCtrl})
      .when('/slaves/:slave_id/frameworks/:framework_id', {template: 'static/slave_framework.html', controller: SlaveCtrl})
      .when('/slaves/:slave_id/frameworks/:framework_id/executors/:executor_id', {template: 'static/slave_executor.html', controller: SlaveCtrl})
      .when('/slaves/:slave_id/browse', {template: 'static/browse.html', controller: BrowseCtrl})
      .otherwise({redirectTo: '/'});
  }])
  .filter('truncateMesosID', function() {
    return function(id) {
      return '…' + id.split('-').splice(3, 3).join('-');
    }
  })
  .filter('truncateMesosState', function() {
    return function(state) {
      // Remove the "TASK_" prefix.
      return state.substring(5);
    }
  })
  .filter('mesosDate', function($filter) {
    return function(date) {
      return $filter('date')(date, 'MM/dd/yyyy H:mm:ss');
    }
  })
  .filter('relativeDate', function() {
    return function(date) {
      return relativeDate(date);
    }
  })
  .filter('unixDate', function($filter) {
    return function(date) {
      if ((new Date(date)).getFullYear() == (new Date()).getFullYear()) {
        return $filter('date')(date, 'MMM dd HH:mm');
      } else {
        return $filter('date')(date, 'MMM dd YYYY');
      }
    }
  })
  // TODO(bmahler): Replace all mem / disk GB with this filter.
  .filter('dataSize', function() {
    return function(bytes) {
      if (bytes < 1024) {
        return bytes.toFixed() + "B";
      } else if (bytes < (1024 * 1024)) {
        return (bytes / 1024).toFixed() + "K";
      } else if (bytes < (1024 * 1024 * 1024)) {
        return (bytes / (1024 * 1024)).toFixed() + "M";
      } else {
        return (bytes / (1024 * 1024 * 1024)).toFixed() + "G";
      }
    }
  })
  // Defines the ui-if tag. This removes/adds an element from the DOM depending on a condition
  // Originally created by @tigbro, for the @jquery-mobile-angular-adapter
  // https://github.com/tigbro/jquery-mobile-angular-adapter
  .directive('uiIf', [function () {
    return {
      transclude: 'element',
      priority: 1000,
      terminal: true,
      restrict: 'A',
      compile: function (element, attr, linker) {
        return function (scope, iterStartElement, attr) {
          iterStartElement[0].doNotMove = true;
          var expression = attr.uiIf;
          var lastElement;
          var lastScope;
          scope.$watch(expression, function (newValue) {
            if (lastElement) {
              lastElement.remove();
              lastElement = null;
            }
            if (lastScope) {
              lastScope.$destroy();
              lastScope = null;
            }
            if (newValue) {
              lastScope = scope.$new();
              linker(lastScope, function (clone) {
                lastElement = clone;
                iterStartElement.after(clone);
              });
            }
            // Note: need to be parent() as jquery cannot trigger events on comments
            // (angular creates a comment node when using transclusion, as ng-repeat does).
            iterStartElement.parent().trigger("$childrenChanged");
          });
        };
      }
    };
  }]);


function setNavbarActiveTab(tab_name) {
  $('#navbar li').removeClass('active');
  $('#navbar li[data-tabname='+tab_name+']').addClass('active');
}