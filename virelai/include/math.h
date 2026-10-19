#ifndef VIRELAI_MATH_H
#define VIRELAI_MATH_H

/* The engine's live code needs only fabs/fabsf; the trig declarations are
 * here for completeness — nothing in the linked set calls them. */

static inline double fabs(double x) { return x < 0.0 ? -x : x; }
static inline float  fabsf(float x) { return x < 0.0f ? -x : x; }

double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double pow(double x, double y);

#define M_PI 3.14159265358979323846

#endif
