#pragma once

#define csafe_release(p) \
  do                    \
  {                     \
    if(p)               \
    {                   \
      (p)->lpVtbl->Release(p);   \
      (p) = NULL;       \
    }                   \
  } while((void)0, 0)

// benchmarking
#define microsecond 1000000
#define millisecond 1000
extern double g_cpu_frequency;
extern double g_gpu_frequency;
static const struct measurement_s {
	double start_time;
	double end_time;
	double elapsed_ms;
} measurement_default = {.start_time = 0.0, .end_time = 0.0, .elapsed_ms = 0.0};
typedef struct measurement_s measurement;

