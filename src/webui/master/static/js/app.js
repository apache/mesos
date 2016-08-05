(function() {
  'use strict';

  angular.module('mesos', ['ngRoute', 'mesos.services', 'ui.bootstrap', 'ui.bootstrap.dialog']).
    config(['paginationConfig', '$routeProvider', function(paginationConfig, $routeProvider) {
      $routeProvider
        .when('/',
          {templateUrl: 'static/home.html', controller: 'HomeCtrl'})
        .when('/frameworks',
          {templateUrl: 'static/frameworks.html', controller: 'FrameworksCtrl'})
        .when('/frameworks/:id',
          {templateUrl: 'static/framework.html', controller: 'FrameworkCtrl'})
        .when('/offers',
          {templateUrl: 'static/offers.html', controller: 'OffersCtrl'})
        .when('/agents',
          {templateUrl: 'static/agents.html', controller: 'AgentsCtrl'})
        .when('/agents/:agent_id',
          {templateUrl: 'static/agent.html', controller: 'AgentCtrl'})
        .when('/agents/:agent_id/frameworks/:framework_id',
          {templateUrl: 'static/agent_framework.html', controller: 'AgentFrameworkCtrl'})
        .when('/agents/:agent_id/frameworks/:framework_id/executors/:executor_id',
          {templateUrl: 'static/agent_executor.html', controller: 'AgentExecutorCtrl'})

        // TODO(tomxing): Remove the following '/slaves/*' paths once the
        // slave->agent rename is complete(MESOS-3779).
        .when('/slaves', {redirectTo: '/agents'})
        .when('/slaves/:agent_id', {redirectTo: '/agents/:agent_id'})
        .when('/slaves/:agent_id/frameworks/:framework_id',
          {redirectTo: '/agents/:agent_id/frameworks/:framework_id'})
        .when('/slaves/:agent_id/frameworks/:framework_id/executors/:executor_id',
          {redirectTo: '/agents/:agent_id/frameworks/:framework_id/executors/:executor_id'})

        // Use a non-falsy template so the controller will still be executed.
        // Since the controller is intended only to redirect, the blank template
        // is fine.
        //
        // By design, controllers currently will not handle routes if the
        // template is falsy. There is an issue open in Angular to add that
        // feature:
        //
        //     https://github.com/angular/angular.js/issues/1838
        .when('/agents/:agent_id/frameworks/:framework_id/executors/:executor_id/browse',
          {template: ' ', controller: 'AgentExecutorRerouterCtrl'})
        .when('/agents/:agent_id/browse',
          {templateUrl: 'static/browse.html', controller: 'BrowseCtrl'})

        // TODO(tomxing): Remove the following '/slaves/*' paths once the
        // slave->agent rename is complete(MESOS-3779).
        .when('/slaves/:agent_id/frameworks/:framework_id/executors/:executor_id/browse',
          {redirectTo: '/agents/:agent_id/frameworks/:framework_id/executors/:executor_id/browse'})
        .when('/slaves/:agent_id/browse',
          {redirectTo: '/agents/:agent_id/browse'})
        .otherwise({redirectTo: '/'});

      // Configure [Angular UI Pagination][1]:
      //   * Show first/last buttons
      //   * Show 50 items per page
      //   * Show "..." when there are pages beyond the max shown
      //
      // [1] http://angular-ui.github.io/bootstrap/#/pagination
      paginationConfig.boundaryLinks = true;
      paginationConfig.rotate = false;

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
        var i = parseInt(date, 10);
        if (_.isNaN(i)) { return '' };
        return $filter('date')(i, 'yyyy-MM-ddTH:mm:ssZ');
      };
    })
    .filter('relativeDate', function() {
      return function(date, refDate) {
        var i = parseInt(date, 10);
        if (_.isNaN(i)) { return '' };
        return relativeDate(i, refDate);
      };
    })
    .filter('slice', function() {
      return function(array, begin, end) {
        if (_.isArray(array)) {
          return array.slice(begin, end);
        }
      };
    })
    .filter('unixDate', function($filter) {
      return function(date) {
        if ((new Date(date)).getFullYear() == (new Date()).getFullYear()) {
          return $filter('date')(date, 'MMM dd HH:mm');
        } else {
          return $filter('date')(date, 'MMM dd yyyy');
        }
      };
    })
    .filter('dataSize', function() {
      var BYTES_PER_KB = Math.pow(2, 10);
      var BYTES_PER_MB = Math.pow(2, 20);
      var BYTES_PER_GB = Math.pow(2, 30);
      var BYTES_PER_TB = Math.pow(2, 40);
      var BYTES_PER_PB = Math.pow(2, 50);
      // NOTE: Number.MAX_SAFE_INTEGER is 2^53 - 1

      return function(bytes) {
        if (bytes == null || isNaN(bytes)) {
          return '';
        } else if (bytes < BYTES_PER_KB) {
          return bytes.toFixed() + ' B';
        } else if (bytes < BYTES_PER_MB) {
          return (bytes / BYTES_PER_KB).toFixed() + ' KB';
        } else if (bytes < BYTES_PER_GB) {
          return (bytes / BYTES_PER_MB).toFixed() + ' MB';
        } else if (bytes < BYTES_PER_TB) {
          return (bytes / BYTES_PER_GB).toFixed(1) + ' GB';
        } else if (bytes < BYTES_PER_PB) {
          return (bytes / BYTES_PER_TB).toFixed(1) + ' TB';
        } else {
          return (bytes / BYTES_PER_PB).toFixed(1) + ' PB';
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
        template: '<i class="glyphicon glyphicon-file"></i>',

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
    }])
    .directive('mTimestamp', [ '$rootScope', function($rootScope) {
      return {
        restrict: 'E',
        transclude: true,
        scope: {
          value: '@'
        },
        link: function($scope, element, attrs) {
          $scope.longDate = JSON.parse(
            localStorage.getItem('longDate') || false);

          $scope.$on('mTimestamp.toggle', function() {
            $scope.longDate = !$scope.longDate;
          });

          $scope.toggle = function() {
            localStorage.setItem('longDate', !$scope.longDate);
            $rootScope.$broadcast('mTimestamp.toggle');
          };
        },
        templateUrl: 'static/directives/timestamp.html'
      }
    }])
    .directive('mPagination', function() {
      return { templateUrl: 'static/directives/pagination.html' }
    })
    .directive('mTableHeader', function() {
      return { templateUrl: 'static/directives/tableHeader.html' }
    })
    .directive('mTable', ['$compile', '$filter', function($compile, $filter) {
      /* This directive does not have a template. The DOM doesn't like
       * having partially defined tables and so they don't work well with
       * directives and templates. Because of this, the sub-elements that this
       * includes are their own directive/templates and it adds them via. DOM
       * manipulation here.
       */
      return {
        scope: true,
        link: function(scope, element, attrs) {
          var defaultOrder = true;

          _.extend(scope, {
            originalData: [],
            columnKey: '',
            sortOrder: defaultOrder,
            pgNum: 1,
            pageLength: 50,
            filterTerm: '',
            headerTitle: attrs.title
          })
          // ---

          // --- Allow sorting by column based on the <th> data-key attr
          var th = element.find('th');
          th.attr('ng-click', 'sortColumn($event)');
          $compile(th)(scope);

          var setSorting = function(el) {
            var key = el.attr('data-key');

            if (scope.columnKey === key) {
              scope.sortOrder = !scope.sortOrder;
            }
            else { scope.sortOrder = defaultOrder }

            scope.columnKey = key;

            th.removeClass('descending ascending');
            el.addClass(scope.sortOrder ? 'descending' : 'ascending');
          };

          var defaultSortColumn = function() {
            var el = element.find('[data-sort]');
            if (el.length === 0) {
              el = element.find('th:first');
            }
            return el;
          };

          scope.sortColumn = function(ev) {
            setSorting(angular.element(ev.target));
          };

          setSorting(defaultSortColumn());
          // ---

          scope.$watch(attrs.tableContent, function(data) {
            if (!data) { scope.originalData = []; return };
            if (angular.isObject(data)) { data = _.values(data) }

            scope.originalData = data;
          });

          var setTableData = function() {
            scope.filteredData = $filter('filter')(scope.originalData, scope.filterTerm)
            scope.$data = $filter('orderBy')(
              scope.filteredData,
              scope.columnKey,
              scope.sortOrder).slice(
                (scope.pgNum - 1) * scope.pageLength,
                scope.pgNum * scope.pageLength);
          };

          // Reset the page number for each new filtering.
          scope.$watch('filterTerm', function() { scope.pgNum = 1; });

          _.each(['originalData', 'columnKey', 'sortOrder', 'pgNum', 'filterTerm'],
            function(k) { scope.$watch(k, setTableData); });

          // --- Pagination controls
          var el = angular.element('<div m-pagination></div>');
          $compile(el)(scope);
          element.after(el);
          // ---

          // --- Filtering
          var el = angular.element('<div m-table-header></div>');
          $compile(el)(scope);
          element.before(el);
          // ---
        }
      };
     }]);
})();
