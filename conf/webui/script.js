angular
  .module('webui', [])
  .controller('MainCtrl', function($scope, $http) {
    (function updateApi() {
      $http.get('/cache/stats').success(function(data) {
        console.log(data);
        var json = JSON.stringify(data, null, 4);
        $scope.api = json;
        setTimeout(updateApi, 1000);
      });
    })();
  });
