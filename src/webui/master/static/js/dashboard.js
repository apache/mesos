var horizons = {
  create: function(context, name, metric, extent, units) {
    // Add a "graph-container".
    d3.select("#graphs")
    .append("div")
    .attr("id", name + "-container")
    .attr("class", "graph-container");

    // Add a header.
    d3.select("#" + name + "-container")
    .append("h4")
    .text(name);

    // Add a "graph".
    d3.select("#" + name + "-container")
    .append("div")
    .attr("id", name)
    .attr("class", "graph");

    // Add the bottom axis.
    d3.select("#" + name + "-container")
    .select("#" + name)
    .append("div")
    .attr("id", "bottom-axis")
    .attr("class", "bottom axis")
    .call(context.axis().ticks(12).orient("bottom"));

    // Add the rule.
    d3.select("#" + name + "-container")
      .select("#" + name)
      .append("div")
      .attr("class", "rule")
      .call(context.rule());

    var horizon = context.horizon()
      .title(name)
      .height(30)
      .extent(extent)
      .metric(metric)
      .format(function (value) { return d3.format(".3g")(value) + " " + units; });

    function clicked(datum, index) {
      // Remove the graph from the dashboard.
      d3.select("#" + name + "-container")
          .select("#" + name)
          .select("#" + name + "-horizon")
          .call(horizon.remove)
          .remove();

      // Now increase it or decrease it.
      if (!d3.event.shiftKey) {
          // Increase size.
          horizon = context.horizon()
              .height(horizon.height() * 2)
              .extent([horizon.extent()[0] * 2, horizon.extent()[1] * 2])
              .metric(metric)
              .format(function (value) { return d3.format(".3g")(value) + " " + units; });
          horizon(
            d3.select("#" + name + "-container")
              .select("#" + name)
              .insert("div", ".bottom")
              .attr("class", "horizon")
              .attr("id", name + "-horizon")
              .style("cursor", "pointer")
              .on("click", clicked));
      } else {
          // Decrease size.
          horizon = context.horizon()
            .height(horizon.height() / 2)
            .extent([horizon.extent()[0] / 2, horizon.extent()[1] / 2])
            .metric(metric)
            .format(function (value) { return d3.format(".3g")(value) + " " + units; });
          horizon(
            d3.select("#" + name + "-container")
              .select("#" + name)
              .insert("div", ".bottom")
              .attr("class", "horizon")
              .attr("id", name + "-horizon")
              .style("cursor", "pointer")
              .on("click", clicked));
      }
    }

    horizon(
      d3.select("#" + name + "-container")
        .select("#" + name)
         .insert("div", ".bottom")
         .attr("class", "horizon")
         .attr("id", name + "-horizon")
         .style("cursor", "pointer")
         .on("click", clicked));

    context.on("focus", function(i) {
      d3.selectAll(".value").style("right", i == null ? null : context.size() - i + "px");
    });

    return context;
  }
};


function metric_cpus(context) {
  return context.metric(function(start, stop, step, callback) {
    // Convert the start and stop "dates" into milliseconds.
    start = +start, stop = +stop;

    var values = [];
    _((stop - start) / step).times(function() {
      values.push(4);
    });

    // Return the data requested.
    callback(null, values);
  }, "cpus");
}


function metric_mem(context) {
  return context.metric(function(start, stop, step, callback) {
    // Convert the start and stop "dates" into milliseconds.
    start = +start, stop = +stop;

    var values = [];
    _((stop - start) / step).times(function() {
      values.push(34);
    });

    // Return the data requested.
    callback(null, values);
  }, "mem");
}


function random(context, name) {
  var value = 0,
      values = [],
      i = 0,
      last;
  return context.metric(function(start, stop, step, callback) {
    start = +start, stop = +stop;
    if (isNaN(last)) last = start;
    while (last < stop) {
        last += step;
        value = Math.abs(value + .8 * Math.random() - .4 + .2 * Math.cos(i += .2));
        values.push(value);
    }
    callback(null, values = values.slice((start - stop) / step));
  }, name);
}
