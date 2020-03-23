#ifndef GTPRAGMA_H_
#define GTPRAGMA_H_

/* C99 or C++11 */
#if (( __STDC_VERSION__ >= 199901L ) || (__cplusplus >= 201103L ))
# define PRAGMA(x) _Pragma(#x)

/* OpenMP 4+ */
# if defined(_OPENMP) && (_OPENMP >= 201307)
#  define PRAGMA_SIMD PRAGMA(omp simd)

/* Intel */
# elif defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 1800)
#  define PRAGMA_SIMD PRAGMA(ivdep) /* PRAGMA(vector always) might be better... */
# elif defined(__INTEL_COMPILER) && (__INTEL_COMPILER < 1800)
#  define PRAGMA_SIMD PRAGMA(simd)

/* Unsupported */
# else
#  warning No definition of PRAGMA_SIMD for your compiler...
#  define PRAGMA_SIMD
# endif

/* C90 or older */
#else
# warning Without a C99/C++11 compiler, PRAGMA_SIMD does nothing.
# define PRAGMA_SIMD
#endif

#endif // GTPRAGMA_H_
