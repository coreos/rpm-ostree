(function(exports) {
    'use strict';

    var rpmostreeControllers = angular.module('rpmostreeControllers', []);

    var ROOT = '/';

    rpmostreeControllers.controller('rpmostreeHomeCtrl', function($scope, $http) {
        var builds = [];

        $http.get(ROOT + 'autobuilder-status.json').success(function(status) {
            var text;
            if (status.running.length > 0)
                text = 'Running: ' + status.running.join(' ') + '; load=' + status.systemLoad[0];
            else
                text = 'Idle, awaiting packages';

            $scope.runningState = text;
        });

        var productsBuiltPath = ROOT + 'results/tasks/build/products-built.json';
	console.log("Retrieving products-built.json");
	$http.get(productsBuiltPath).success(function(data) {
	    var trees = data['trees'];
	    for (var ref in trees) {
		var treeData = trees[ref];
		var refUnix = ref.replace(/\//g, '-');
		treeData.refUnix = refUnix;
		treeData.screenshotUrl = '/results/tasks/smoketest/smoketest/work-' + refUnix + '/screenshot-final.png';
	    }
	    $scope.trees = trees;
	});
    });

})(window);
