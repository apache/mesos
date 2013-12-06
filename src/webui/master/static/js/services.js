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
})();
