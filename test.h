#ifndef __TESTS_H_
#define __TESTS_H_

static int tests_failed = 0;
static int tests_total = 0;

int err(int cond, char *msg) {
  ++tests_total;
  if (cond)
    printf("%dth test failed: %s\n", ++tests_failed, msg);

  return cond;
}

int get_failed() { return tests_failed; }
int get_total() { return tests_total; }
int get_passed() { return tests_total - tests_failed; }

#endif
