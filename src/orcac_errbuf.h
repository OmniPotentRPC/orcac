#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static inline void orcac_errf(char *errbuf, size_t errbuf_n, const char *fmt,
                              ...) {
  va_list ap;
  if (errbuf == NULL || errbuf_n == 0) {
    return;
  }
  va_start(ap, fmt);
  vsnprintf(errbuf, errbuf_n, fmt, ap);
  va_end(ap);
}
