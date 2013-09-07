// graph val, it queries it every second and draws
// the last 20 secs, it also takes draw as an optional attribute and only
// draws the graph when it is true, if not given then graph is always drawn
angular
.module('webui.directives.dgraph', ['webui.services.deps'])
.directive('dgraph', ['$', function($) {
  return function(scope, elem, attrs) {
    var canDraw = true;

    var graphSize = 20
      , curr_val = 0
      , conf = {
        label: attrs.label,
        data: [],
        color: "#ff0000",
        lines: { show: true }
      },
      start = new Date
    ;


    // hack for the null height for flot elem
    elem.height(elem.width() / 2);

    var graph = $.plot(elem, [conf], {
      legend: { show: true },
      xaxis: {
        show: true
      },
      yaxis: {
        show: true
      }
    });

    var draw = function() {
      var width = elem.width();
      if (width == 0) return;

      elem.height(width / 2);

      graph.setData([conf]);
      graph.resize();
      graph.setupGrid();
      graph.draw();
    };

    function update() {
      var cnt = ((new Date - start)/1000).toFixed(0);

      conf.data.push([cnt, curr_val]);
      if (conf.data.length > graphSize) conf.data.shift();

      // if any parents is collapsable, then confirm if it isnt
      if (canDraw)
        draw();
    };

    scope.$watch(attrs.val, function(val) {
      curr_val = parseFloat(val) || 0;
    });

    scope.$watch(attrs.label, function(label) {
      conf.label = label;
    });

    if (attrs.draw) {
      scope.$watch(attrs.draw, function(val) {
        canDraw = val;
      });
    }

    var interval = setInterval(update, 1000);

    angular.element(window).bind('resize', draw);
    elem.bind('$destroy', function() {
      clearInterval(interval);
    });

  };
}]);
