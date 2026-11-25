#include "orcac_locale.h"

#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_once_t kLocaleOnce = PTHREAD_ONCE_INIT;
static locale_t kCLocale = (locale_t)0;

static void init_c_locale(void) {
  kCLocale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
}

static locale_t enter_c_locale(void) {
  locale_t old = (locale_t)0;
  pthread_once(&kLocaleOnce, init_c_locale);
  if (kCLocale != (locale_t)0) {
    old = uselocale(kCLocale);
  }
  return old;
}

static void leave_c_locale(locale_t old) {
  if (old != (locale_t)0) {
    uselocale(old);
  }
}

int orcac_vsnprintf_c(char *dst, size_t dst_n, const char *fmt, va_list ap) {
  locale_t old = enter_c_locale();
  int rc = vsnprintf(dst, dst_n, fmt, ap);
  leave_c_locale(old);
  return rc;
}

int orcac_strtod_c(const char *text, char **endptr, double *value_out) {
  locale_t old = enter_c_locale();
  char *end = NULL;
  double value = strtod(text, &end);
  leave_c_locale(old);
  if (endptr != NULL) {
    *endptr = end;
  }
  if (value_out != NULL) {
    *value_out = value;
  }
  return 0;
}
