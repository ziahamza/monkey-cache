angular
  .module('webui.ctrls.main', ['webui.services.deps'])
  .controller('MainCtrl', function($scope, $json, $http) {
    $scope.files = [];
    $scope.memory = {
      pipe_mem_used: 0
    };

    $scope.requests = {
      served_per_sec: 0
    }
    $scope.reset = function(uri) {
      $http.get('/cache/reset' + uri).success(function(data) {
        console.log(data);
        $scope.files = $scope.files.filter(function(f) {
          return f.uri != uri;
        });
      });
    };

    (function updateApi() {
      $http.get('/cache/stats').success(function(data) {
        var json = $json.stringify(data, null, 4);
        $scope.api = json;
        $scope.files = data.files;
        $scope.memory = data.memory;
        $scope.requests = data.requests;
        setTimeout(updateApi, 1000);
      });
    })();
  });
