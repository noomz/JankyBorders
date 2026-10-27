#include "../src/misc/radius.h"
#include <stdio.h>
#include <stdlib.h>

static int g_failed;

static void expect_float(const char *name, float got, float want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %g want %g\n", name, got, want);
    g_failed = 1;
  }
}

static void expect_true(const char *name, bool ok) {
  if (!ok) {
    fprintf(stderr, "FAIL %s\n", name);
    g_failed = 1;
  }
}

int main(void) {
  expect_float("missing api uses default 9",
               border_radius_from_api(false, 0.f), 9.f);
  expect_float("reported 16 is kept",
               border_radius_from_api(true, 16.f), 16.f);
  expect_float("reported 1 is kept",
               border_radius_from_api(true, 1.f), 1.f);
  expect_float("reported 0 stays square",
               border_radius_from_api(true, 0.f), 0.f);

  expect_float("inner follows rounded radius",
               border_inner_radius(16.f), 17.f);
  expect_float("inner of square is square",
               border_inner_radius(0.f), 0.f);

  float parsed = -1.f;
  double raw = 0.0;
  CFNumberRef zero = CFNumberCreate(NULL, kCFNumberDoubleType, &raw);
  expect_true("float 0 parses", border_cfnumber_as_radius(zero, &parsed));
  expect_float("float 0 is 0 not missing", parsed, 0.f);
  CFRelease(zero);

  parsed = -1.f;
  raw = 16.0;
  CFNumberRef sixteen = CFNumberCreate(NULL, kCFNumberDoubleType, &raw);
  expect_true("float 16 parses", border_cfnumber_as_radius(sixteen, &parsed));
  expect_float("float 16 is 16", parsed, 16.f);
  CFRelease(sixteen);

  if (g_failed) return 1;
  printf("ok\n");
  return 0;
}
