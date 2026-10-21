// Host stand-in for <android/log.h>, for running the proxy's tests on Linux.
#pragma once
#include <cstdarg>
#include <cstdio>
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
inline int __android_log_print(int priority, const char* tag, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  std::fprintf(stderr, "[%s %d] ", tag, priority);
  std::vfprintf(stderr, format, arguments);
  std::fputc('\n', stderr);
  va_end(arguments);
  return 0;
}
