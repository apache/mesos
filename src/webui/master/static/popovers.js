// An abstraction for handling popovers.
var Popovers = {
  popover: false,

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

      if (Popovers.popover && event.target.rel != 'popover') {
        Popovers.hide();
      }
    });
  },

  show: function(event, p) {
    Popovers.hide(); // Hide any popovers if some are currently shown.

    var target = $(event.target);
    target.popover({
      html: true,
      placement: p,
      trigger: 'manual'
    });

    target.popover('show');
    Popovers.popover = true;
    // TODO(benh): event.preventDefault();
    },

  hide: function() {
    // We can't just keep a reference to the element that triggered
    // the popover because the DOM is constantly changing (thanks to
    // AngularJS) and so the best we can do is just remove (i.e.,
    // hide) all "popovers".
    $('.popover').each(function() {
      $(this).remove();
    });
    Popovers.popover = false;
  }
}
