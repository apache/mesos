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
      var short_id =  id.split('-').splice(2,2).join('-');
      return '…' + short_id;
    }
  });

function setNavbarActiveTab(tab_name) {
  $('#navbar li').removeClass('active');
  $('#navbar li[data-tabname='+tab_name+']').addClass('active');
}