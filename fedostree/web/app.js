(function(exports) {
    'use strict';

    var rpmostree = angular.module('rpm-ostree', [
        'ngRoute',
        'rpmostreeControllers',
    ]);

    rpmostree.config(['$routeProvider', function($routeProvider) {
        $routeProvider.
            when('/', {
                templateUrl: 'partials/home.html'
            }).
            when('/installation', {
                templateUrl: 'partials/installation.html'
            }).
            otherwise({
                redirectTo: 'partials/unknown.html',
            });
    }]);

})(window);
