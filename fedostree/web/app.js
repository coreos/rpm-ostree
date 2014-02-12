(function(exports) {
    'use strict';

    var rpmostree = angular.module('rpm-ostree', [
        'ngRoute',
        'rpmostreeControllers',
    ]);

    rpmostree.config(['$routeProvider', function($routeProvider) {
        $routeProvider.
            when('/', {
                templateUrl: 'partials/home.html',
                controller: 'rpmostreeHomeCtrl'
            }).
            when('/installation', {
                templateUrl: 'partials/installation.html'
            }).
            when('/background', {
                templateUrl: 'partials/background.html'
            }).
            when('/implementation', {
                templateUrl: 'partials/implementation.html'
            }).
            otherwise({
                redirectTo: 'partials/unknown.html',
            });
    }]);

})(window);
