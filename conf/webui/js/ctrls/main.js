angular
  .module('webui.ctrls.main', [])
  .controller('MainCtrl', function($scope, $http) {
    $scope.files = [];
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
        var json = JSON.stringify(data, null, 4);
        $scope.api = json;
        $scope.files = data.files;
        setTimeout(updateApi, 1000);
      });
    })();
  });
