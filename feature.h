/* Copyright (c) 2019-2021, Evgeny Baskov. All rights reserved */

#ifndef FEATURES_H_
#define FEATURES_H_ 1

/* This allows better frames timing */
#define USE_PPOLL 1

/* Ability to override box drawing characters */
#define USE_BOXDRAWING 1

/* X11 Backend (MIT-SHM/XRender) */
#define USE_X11SHM 1

/* Unicode combining characters precomposition */
#define USE_PRECOMPOSE 1

/* Use posix shm interface */
#define USE_POSIX_SHM 1

/* URI openning/parsing/highlighting */
#define USE_URI 1

/* NSST Version number */
#define NSST_VERSION 20105

#endif
