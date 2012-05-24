// An abstraction for handling popovers.
var Popovers = {
  exist: false, // Indicates whether a popover is currently open or not.

  initialize: function() {
    // Turn off popovers if one is shown and we click on something
    // else (other than some part of the popover itself).
    $(document).click(function(event) {
      var target = $(event.target);

      if (target.parent().is('.popover-content') ||
          target.parent().is('.popover-inner') ||
          target.parent().is('.popover')) {
        return;
      }

      if (Popovers.exist && event.target.rel != 'popover') {
        Popovers.hide();
      }
    });
  },

  show: function(event, placement) {
    Popovers.hide(); // Hide any popovers if some are currently shown.

    var target = $(event.target);
    target.popover({
      html: true,
      placement: placement,
      trigger: 'manual'
    });

    target.popover('show');
    Popovers.exist = true;
    // TODO(benh): event.preventDefault();
    },

  hide: function() {
    // We can't just keep a reference to the element that triggered
    // the popover because the DOM might have changed (e.g., if using
    // something like AngularJS) and so the best we can do is just
    // remove (i.e., hide) all "popovers".
    $('.popover').each(function() {
      $(this).remove();
    });
    Popovers.exist = false;
  }
}
