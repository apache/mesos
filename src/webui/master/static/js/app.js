(function() {
  'use strict';

  angular.module('mesos', ['ngRoute', 'mesos.services', 'ui.bootstrap', 'ui.bootstrap.dialog']).
    config(['paginationConfig', '$routeProvider', function(paginationConfig, $routeProvider) {
      $routeProvider
        .when('/',
          {templateUrl: 'static/home.html', controller: 'HomeCtrl'})
        .when('/agents',
          {templateUrl: 'static/agents.html', controller: 'AgentsCtrl'})
        .when('/agents/:agent_id',
          {templateUrl: 'static/agent.html', controller: 'AgentCtrl'})
        .when('/agents/:agent_id/frameworks/:framework_id',
          {templateUrl: 'static/agent_framework.html', controller: 'AgentFrameworkCtrl'})
        .when('/agents/:agent_id/frameworks/:framework_id/executors/:executor_id',
          {templateUrl: 'static/agent_executor.html', controller: 'AgentExecutorCtrl'})
        .when('/frameworks',
          {templateUrl: 'static/frameworks.html', controller: 'FrameworksCtrl'})
        .when('/frameworks/:id',
          {templateUrl: 'static/framework.html', controller: 'FrameworkCtrl'})
        .when('/maintenance',
          {templateUrl: 'static/maintenance.html', controller: 'MaintenanceCtrl'})
        .when('/offers',
          {templateUrl: 'static/offers.html', controller: 'OffersCtrl'})
        .when('/roles',
          {templateUrl: 'static/roles.html', controller: 'RolesCtrl'})

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
          {template: ' ', controller: 'AgentTaskAndExecutorRerouterCtrl'})
        .when('/agents/:agent_id/frameworks/:framework_id/executors/:executor_id/tasks/:task_id/browse',
          {template: ' ', controller: 'AgentTaskAndExecutorRerouterCtrl'})
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
        return $filter('date')(i, 'yyyy-MM-ddTHH:mm:ssZ');
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
    // A filter that uses to convert small float number to decimal string.
    .filter('decimalFloat', function() {
      return function(num) {
        return num ? parseFloat(num.toFixed(4)).toString() : num;
      }
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
    .directive('clipboard', [function() {
      return {
        restrict: 'A',
        scope: true,
        template: '<i class="glyphicon glyphicon-file"></i>',

        link: function(scope, element, attrs) {
          var clip = new Clipboard(element[0]);

          element.on('mouseenter', function() {
            element.addClass('clipboard-is-hover');
            element.triggerHandler('clipboardhover');
          });

          element.on('mouseleave', function() {
            // Restore tooltip content to its original value if it was
            // changed by this Clipboard instance.
            if (scope && scope.tt_content_orig) {
              scope.tt_content = scope.tt_content_orig;
              delete scope.tt_content_orig;
            }

            element.removeClass('clipboard-is-hover');
            element.triggerHandler('clipboardhover');
          });

          // Success for browsers with `execCommand` support.
          clip.on('success', function () {
            // Store the tooltip's original content so it can
            // be restored when the tooltip is hidden.
            scope.tt_content_orig = scope.tt_content;

            // Angular UI's Tooltip sets content on the element's scope in a
            // variable named 'tt_content'. The Tooltip has no public interface,
            // so set the value directly here to change the value of the tooltip
            // when content is successfully copied.
            scope.tt_content = 'Copied!';
            scope.$apply();
          });

          // Support for all other browsers without `execCommand`
          // support. Text will be selected and user will be prompted
          // to copy.
          clip.on('error', function() {
            scope.tt_content_orig = scope.tt_content;
            scope.tt_content = 'Press Ctrl/Cmd + C to copy!';
            scope.$apply();
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

            // Prevent sorting when 'data-key' is undefined.
            if (!key) {
              return;
            }

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
