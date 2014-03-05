(function(exports) {
    'use strict';

    var rpmostreeControllers = angular.module('rpmostreeControllers', []);

    rpmostreeControllers.controller('rpmostreeHomeCtrl', function($scope, $http, $location) {
        var builds = [];

        $http.get(window.location.pathname + '../autobuilder-status.json').success(function(status) {
            var text;
            if (status.running.length > 0)
                text = 'Running: ' + status.running.join(' ') + '; load=' + status.systemLoad[0];
            else
                text = 'Idle, awaiting packages';

            $scope.runningState = text;
        });

	var latestSmoketestPath = window.location.pathname + '../results/tasks/smoketest';
        var productsBuiltPath = latestSmoketestPath + '/products-built.json';
	console.log("Retrieving products-built.json");
	$http.get(productsBuiltPath).success(function(data) {
	    var trees = data['trees'];
	    for (var ref in trees) {
		var treeData = trees[ref];
		var refUnix = ref.replace(/\//g, '-');
		treeData.refUnix = refUnix;
		treeData.screenshotUrl = latestSmoketestPath + '/smoketest/work-' + refUnix + '/screenshot-final.png';
	    }
	    $scope.trees = trees;
	});
    });

    rpmostreeControllers.controller('rpmostreeRefsCtrl', function($scope, $http, $location) {
        var builds = [];

	$scope.refs = [];

        $http.get(window.location.pathname + '../repoweb-data/refs.json').success(function(data) {
	    var refs = data['refs'];
	    $scope.refs = refs;
        });
    });

    function fetchCommitDataRecurse($scope, $http, revision) {
        $http.get(window.location.pathname + '../repoweb-data/commit-' + revision + '.json').success(function(data) {
	    data['revision'] = revision;
	    var d = new Date();
	    d.setTime(data.t * 1000);
	    data['formattedDate'] = d.toUTCString();
	    $scope.commits.push(data);
	    var parent = data['parent'];
	    if (parent) {
		fetchCommitDataRecurse($scope, $http, parent);
	    }
        });
    }

    rpmostreeControllers.controller('rpmostreeLogCtrl', function($scope, $http, $routeParams) {
	var revision = $routeParams.revision;
	$scope.revision = revision;
	$scope.commits = [];
	fetchCommitDataRecurse($scope, $http, revision);
    });

    rpmostreeControllers.controller('rpmostreeCommitCtrl', function($scope, $http, $routeParams) {
	var revision = $routeParams.revision;
	$scope.revision = revision;
        $http.get(window.location.pathname + '../repoweb-data/commit-' + revision + '.json').success(function(data) {
	    data['revision'] = revision;
	    var d = new Date();
	    d.setTime(data.t * 1000);
	    data['formattedDate'] = d.toUTCString();
	    var commitdata = data;
	    $scope.commit = commitdata;
            $http.get(window.location.pathname + '../repoweb-data/diff-' + revision + '.json').success(function(data) {
		commitdata['difftxt'] = data['difftxt'];
	    });
        });
    });

})(window);
