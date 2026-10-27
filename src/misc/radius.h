#pragma once
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>

static inline bool border_cfnumber_as_radius(CFTypeRef value, float *out) {
  if (!value || !out) return false;
  if (CFGetTypeID(value) != CFNumberGetTypeID()) return false;
  double radius = 0;
  if (!CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, &radius)) {
    return false;
  }
  *out = (float)radius;
  return true;
}

static inline float border_radius_from_api(bool got_radius, float radius) {
  return got_radius ? radius : 9.f;
}

static inline float border_inner_radius(float radius) {
  return radius > 0.f ? radius + 1.f : 0.f;
}
