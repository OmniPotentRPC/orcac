#pragma once

#include <locale.h>

/*
 * Switch the calling thread, and only it, to the C numeric conventions
 * so printf and strtod use '.' whatever setlocale the host has made.
 * Every successful orcac_c_numeric_enter needs one orcac_c_numeric_leave
 * on the same thread, which restores the previous locale and frees the
 * one made here.
 */
typedef struct OrcacCNumeric {
  locale_t c;
  locale_t prev;
} OrcacCNumeric;

static inline int orcac_c_numeric_enter(OrcacCNumeric *s) {
  s->prev = (locale_t)0;
  s->c = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (s->c == (locale_t)0) {
    return -1;
  }
  s->prev = uselocale(s->c);
  if (s->prev == (locale_t)0) {
    freelocale(s->c);
    s->c = (locale_t)0;
    return -1;
  }
  return 0;
}

static inline void orcac_c_numeric_leave(OrcacCNumeric *s) {
  if (s->c == (locale_t)0) {
    return;
  }
  uselocale(s->prev);
  freelocale(s->c);
  s->c = (locale_t)0;
  s->prev = (locale_t)0;
}
