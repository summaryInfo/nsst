#ifndef FEATURES_H_
#define FEATURES_H_ 1

/* This allows better frames timing */

#if defined(__linux__) || defined(__OpenBSD__)
#	define USE_PPOLL
#endif

/* Ability to override box drawing characters */
// #define USE_BOXDRAWING

#endif