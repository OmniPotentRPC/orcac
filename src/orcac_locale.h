#pragma once

#include <stddef.h>
#include <stdarg.h>

/*
 * Locale-independent numeric helpers.
 *
 * These helpers switch the calling thread to the C locale for the
 * duration of each conversion/format call, so decimal parsing and
 * formatting stay stable under non-C LC_NUMERIC settings.
 */
int orcac_vsnprintf_c(char *dst, size_t dst_n, const char *fmt, va_list ap);
int orcac_strtod_c(const char *text, char **endptr, double *value_out);
