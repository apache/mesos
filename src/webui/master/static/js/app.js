(function() {
  'use strict';

  angular.module('mesos', ['ui.bootstrap']).
    config(['$dialogProvider', '$routeProvider', function($dialogProvider, $routeProvider) {
      $routeProvider
        .when('/',
          {templateUrl: 'static/home.html', controller: 'HomeCtrl'})
        .when('/dashboard',
          {templateUrl: 'static/dashboard.html', controller: 'DashboardCtrl'})
        .when('/frameworks',
          {templateUrl: 'static/frameworks.html', controller: 'FrameworksCtrl'})
        .when('/frameworks/:id',
          {templateUrl: 'static/framework.html', controller: 'FrameworkCtrl'})
        .when('/slaves',
          {templateUrl: 'static/slaves.html', controller: 'SlavesCtrl'})
        .when('/slaves/:slave_id',
          {templateUrl: 'static/slave.html', controller: 'SlaveCtrl'})
        .when('/slaves/:slave_id/frameworks/:framework_id',
          {templateUrl: 'static/slave_framework.html', controller: 'SlaveFrameworkCtrl'})
        .when('/slaves/:slave_id/frameworks/:framework_id/executors/:executor_id',
          {templateUrl: 'static/slave_executor.html', controller: 'SlaveExecutorCtrl'})

        // Use a non-falsy template so the controller will still be executed.
        // Since the controller is intended only to redirect, the blank template
        // is fine.
        //
        // By design, controllers currently will not handle routes if the
        // template is falsy. There is an issue open in Angular to add that
        // feature:
        //
        //     https://github.com/angular/angular.js/issues/1838
        .when('/slaves/:slave_id/frameworks/:framework_id/executors/:executor_id/browse',
          {template: ' ', controller: 'SlaveExecutorRerouterCtrl'})
        .when('/slaves/:slave_id/browse',
          {templateUrl: 'static/browse.html', controller: 'BrowseCtrl'})
        .otherwise({redirectTo: '/'});

      $dialogProvider.options({dialogFade: true});

      ZeroClipboard.setDefaults({
        moviePath: '/static/obj/zeroclipboard-1.1.7.swf'
      });
    }])
    .filter('truncateMesosID', function() {
      return function(id) {
        if (id) {
          var truncatedIdParts = id.split('-');

          if (truncatedIdParts.length > 3) {
            return '…' + truncatedIdParts.splice(3, 3).join('-');
          } else {
            return id;
          }
        } else {
          return '';
        }
      };
    })
    .filter('truncateMesosState', function() {
      return function(state) {
        // Remove the "TASK_" prefix.
        return state.substring(5);
      };
    })
    .filter('isoDate', function($filter) {
      return function(date) {
        return $filter('date')(date, 'yyyy-MM-dd H:mm:ss');
      };
    })
    .filter('relativeDate', function() {
      return function(date) {
        return relativeDate(date);
      };
    })
    .filter('unixDate', function($filter) {
      return function(date) {
        if ((new Date(date)).getFullYear() == (new Date()).getFullYear()) {
          return $filter('date')(date, 'MMM dd HH:mm');
        } else {
          return $filter('date')(date, 'MMM dd YYYY');
        }
      };
    })
    .filter('dataSize', function() {
      return function(bytes) {
        if (bytes === null || bytes === undefined || isNaN(bytes)) {
          return '';
        } else if (bytes < 1024) {
          return bytes.toFixed() + ' B';
        } else if (bytes < (1024 * 1024)) {
          return (bytes / 1024).toFixed() + ' KB';
        } else if (bytes < (1024 * 1024 * 1024)) {
          return (bytes / (1024 * 1024)).toFixed() + ' MB';
        } else {
          return (bytes / (1024 * 1024 * 1024)).toFixed() + ' GB';
        }
      };
    })
    // Defines the 'clipboard' directive, which integrates copying to the user's
    // clipboard with an Adobe Flash object via the ZeroClipboard library.
    //
    // Text to be copied on click is specified with the 'data-clipboard-text'
    // attribute.
    //
    // The 'mouseenter' and 'mouseleave' events from the Flash object are exposed
    // to the directive's element via the 'clipboardhover' event. There is no
    // differentiation between enter/leave; they are both called 'clipboardhover'.
    //
    // Example:
    //
    //     <button class="btn btn-mini" clipboard
    //         data-clipboard-text="I'm in your clipboard!">
    //     </button>
    //
    // See: http://zeroclipboard.github.io/ZeroClipboard/
    .directive('clipboard', [function() {
      return {
        restrict: 'A',
        scope: true,
        template: '<i class="icon-file"></i>',

        link: function(scope, element, attrs) {
          var clip = new ZeroClipboard(element[0]);

          clip.on('mouseover', function() {
            angular.element(this).triggerHandler('clipboardhover');
          });

          clip.on('mouseout', function() {
            // TODO(ssorallen): Why is 'scope' incorrect here? It has to be
            // retrieved from the element explicitly to be correct.
            var elScope = angular.element(this).scope();

            // Restore tooltip content to its original value if it was changed by
            // this Clipboard instance.
            if (elScope && elScope.tt_content_orig) {
              elScope.tt_content = elScope.tt_content_orig;
              delete elScope.tt_content_orig;
            }

            angular.element(this).triggerHandler('clipboardhover');
          });

          clip.on('complete', function() {
            // TODO(ssorallen): Why is 'scope' incorrect here? It has to be
            // retrieved from the element explicitly to be correct.
            var elScope = angular.element(this).scope();

            if (elScope) {
              // Store the tooltip's original content so it can be restored when
              // the tooltip is hidden.
              elScope.tt_content_orig = elScope.tt_content;

              // Angular UI's Tooltip sets content on the element's scope in a
              // variable named 'tt_content'. The Tooltip has no public interface,
              // so set the value directly here to change the value of the tooltip
              // when content is successfully copied.
              elScope.tt_content = 'copied!';
              elScope.$apply();
            }
          });

          clip.on('load', function() {
            // The 'load' event fires only if the Flash file loads successfully.
            // The copy buttons will only display if the class 'flash' exists
            // on an ancestor.
            //
            // Browsers with no flash support will not append the 'flash' class
            // and will therefore not see the copy buttons.
            angular.element('html').addClass('flash');
          });
        }
      };
    }]);
})();
