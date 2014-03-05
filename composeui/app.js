(function(exports) {
    'use strict';

    var rpmostree = angular.module('rpmostree', [
        'ngRoute',
        'rpmostreeControllers',
    ]);

    rpmostree.config(['$routeProvider', function($routeProvider) {
        $routeProvider.
            when('/', {
                templateUrl: 'partials/home.html',
                controller: 'rpmostreeHomeCtrl'
	    }).
            when('/repoweb', {
                templateUrl: 'partials/repoweb-overview.html',
                controller: 'rpmostreeRefsCtrl'
	    }).
            when('/repoweb/log/:revision', {
                templateUrl: 'partials/repoweb-log.html',
                controller: 'rpmostreeLogCtrl'
	    }).
            when('/repoweb/commit/:revision', {
                templateUrl: 'partials/repoweb-commit.html',
                controller: 'rpmostreeCommitCtrl'
	    }).
            otherwise({
                redirectTo: 'partials/unknown.html',
            });
    }]);

})(window);
