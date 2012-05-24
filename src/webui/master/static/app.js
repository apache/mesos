'use strict';

angular.module('mesos', []).
  config(['$routeProvider', function($routeProvider) {
    $routeProvider
      .when('/', {template: 'static/home.html', controller: HomeCtrl})
      .when('/dashboard', {template: 'static/dashboard.html', controller: DashboardCtrl})
      .when('/log', {template: 'static/log.html', controller: LogCtrl})
      .when('/frameworks', {template: 'static/frameworks.html', controller: FrameworksCtrl})
      .when('/framework/:id', {template: 'static/framework.html', controller: FrameworkCtrl})
      .when('/slaves', {template: 'static/slaves.html', controller: SlavesCtrl})
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
  });

function setNavbarActiveTab(tab_name) {
  $('#navbar li').removeClass('active');
  $('#navbar li[data-tabname='+tab_name+']').addClass('active');
}