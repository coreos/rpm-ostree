(function(exports) {
    'use strict';

    var rpmostreeControllers = angular.module('rpmostreeControllers', []);

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
