#ifndef UTIL_H_
#define UTIL_H_ 1

_Noreturn void die(const char* fmt, ...);
void warn(const char* fmt, ...);
void info(const char* fmt, ...);
void fatal(const char* fmt, ...);

#endif 
