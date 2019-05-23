#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#include "util.h"

_Noreturn void die(const char* fmt, ...){
	va_list args;

	va_start(args, fmt);
	fputs("[\e[31;1mFATAL\e[0m] ", stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n',stderr);
	va_end(args);
	
	exit(EXIT_FAILURE);
}

void fatal(const char* fmt, ...){
	va_list args;

	va_start(args, fmt);
	fputs("[\e[31;1mFATAL\e[0m] ", stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n',stderr);
	va_end(args);
}

void warn(const char* fmt, ...){
	va_list args;

	va_start(args, fmt);
	fputs("[\e[33;1mWARN\e[0m] ", stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n',stderr);
	va_end(args);

}

void info(const char* fmt, ...){
	va_list args;

	va_start(args, fmt);
	fputs("[\e[32;1mINFO\e[0m] ", stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n',stderr);
	va_end(args);

}
