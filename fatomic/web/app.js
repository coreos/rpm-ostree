(function(exports) {
    'use strict';

    var fatomic = angular.module('fatomic', [
        'ngRoute',
        'fatomicControllers',
    ]);

    fatomic.config(['$routeProvider', function($routeProvider) {
        $routeProvider.
            when('/', {
                templateUrl: 'partials/home.html',
                controller: 'fatomicHomeCtrl'
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
