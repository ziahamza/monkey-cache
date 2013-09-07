#include <stdio.h>

#include "test.h"
#include "utils_tests.c"

int main() {
  util_tests();

  if (get_failed() == 0) {
    printf("%d out of %d tests passed! :) \n",
        get_passed(), get_total());
    return 0;
  }
  else {
    printf("unfortunately %d out of %d tests failed! :( \n",
        get_failed(), get_total());
    return -1;
  }
}
