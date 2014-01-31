(function(exports) {
    'use strict';

    var fb2docker = angular.module('fb2docker', [
        'ngRoute',
        'fb2dockerControllers',
    ]);

    fb2docker.config(['$routeProvider', function($routeProvider) {
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
