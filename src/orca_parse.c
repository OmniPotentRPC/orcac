#include "orca_parse.h"
#include "orcac_errbuf.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_text(const char *text) {
  size_t n;
  char *out;
  if (text == NULL) {
    text = "";
  }
  n = strlen(text);
  out = (char *)malloc(n + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, text, n + 1u);
  return out;
}

void orcac_free_engrad(OrcacEngrad *parsed) {
  if (parsed == NULL) {
    return;
  }
  free(parsed->gradient_h_bohr);
  free(parsed->atomic_numbers);
  memset(parsed, 0, sizeof(*parsed));
}

int orcac_read_file(const char *path, char **text_out) {
  FILE *fp;
  long size;
  char *text;
  size_t got;
  if (path == NULL || text_out == NULL) {
    return -1;
  }
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return -1;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  size = ftell(fp);
  if (size < 0) {
    fclose(fp);
    return -1;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }
  text = (char *)malloc((size_t)size + 1u);
  if (text == NULL) {
    fclose(fp);
    return -1;
  }
  got = fread(text, 1u, (size_t)size, fp);
  fclose(fp);
  text[got] = '\0';
  *text_out = text;
  return 0;
}

char *orcac_file_tail(const char *path, size_t max_bytes) {
  FILE *fp;
  long size;
  long keep;
  char *text;
  size_t got;
  if (path == NULL) {
    return dup_text("(no path)");
  }
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return dup_text("(missing)");
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return dup_text("(unreadable)");
  }
  size = ftell(fp);
  if (size <= 0) {
    fclose(fp);
    return dup_text("(empty)");
  }
  keep = (long)max_bytes;
  if (keep <= 0 || keep > size) {
    keep = size;
  }
  if (fseek(fp, size - keep, SEEK_SET) != 0) {
    fclose(fp);
    return dup_text("(unreadable)");
  }
  text = (char *)malloc((size_t)keep + 1u);
  if (text == NULL) {
    fclose(fp);
    return dup_text("(oom)");
  }
  got = fread(text, 1u, (size_t)keep, fp);
  fclose(fp);
  text[got] = '\0';
  return text;
}

/*
 * Next whitespace-separated token, skipping '#' comments. Returns 1 for
 * a token, 0 at the end of text, -1 for a token that does not fit.
 */
static int next_token(const char **cursor, char *token, size_t token_n) {
  const char *p = *cursor;
  size_t n = 0;
  while (*p != '\0') {
    if (*p == '#') {
      while (*p != '\0' && *p != '\n') {
        ++p;
      }
    }
    if (*p == '\0') {
      break;
    }
    if (!isspace((unsigned char)*p)) {
      break;
    }
    ++p;
  }
  if (*p == '\0') {
    *cursor = p;
    return 0;
  }
  while (*p != '\0' && !isspace((unsigned char)*p) && *p != '#') {
    if (n + 1u >= token_n) {
      *cursor = p;
      return -1;
    }
    token[n++] = *p;
    ++p;
  }
  token[n] = '\0';
  *cursor = p;
  return 1;
}

static int parse_int(const char *token, int *out) {
  char *end = NULL;
  long v;
  errno = 0;
  v = strtol(token, &end, 10);
  if (end == token || *end != '\0' || errno == ERANGE || v < INT_MIN ||
      v > INT_MAX) {
    return -1;
  }
  *out = (int)v;
  return 0;
}

/*
 * Decimal or exponent notation only, no hex, inf or nan, and nothing
 * left over. Overflow is an error; underflow keeps the value strtod
 * returns, which is zero or subnormal.
 */
static int parse_double(const char *token, double *out) {
  char *end = NULL;
  double v;
  for (const char *p = token; *p != '\0'; ++p) {
    if (!isdigit((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.' &&
        *p != 'e' && *p != 'E') {
      return -1;
    }
  }
  errno = 0;
  v = strtod(token, &end);
  if (end == token || *end != '\0' || !isfinite(v) ||
      (errno == ERANGE && fabs(v) > 1.0)) {
    return -1;
  }
  *out = v;
  return 0;
}

static int read_int(const char **cur, int *out) {
  char token[128];
  return next_token(cur, token, sizeof(token)) == 1 &&
                 parse_int(token, out) == 0
             ? 0
             : -1;
}

static int read_double(const char **cur, double *out) {
  char token[128];
  return next_token(cur, token, sizeof(token)) == 1 &&
                 parse_double(token, out) == 0
             ? 0
             : -1;
}

int orcac_parse_engrad(const char *text,
                       int expected_atoms,
                       OrcacEngrad *parsed,
                       char *errbuf,
                       size_t errbuf_n) {
  const char *cur = text;
  int n_atoms;
  int i;
  if (text == NULL || parsed == NULL || expected_atoms <= 0) {
    return -1;
  }
  memset(parsed, 0, sizeof(*parsed));

  if (read_int(&cur, &n_atoms) != 0) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: malformed engrad: missing or bad atom count");
    return -1;
  }
  if (n_atoms != expected_atoms) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: malformed engrad: atom count %d does not match %d",
               n_atoms, expected_atoms);
    return -1;
  }
  parsed->n_atoms = n_atoms;

  if (read_double(&cur, &parsed->energy_h) != 0) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: malformed engrad: missing or bad energy");
    return -1;
  }

  parsed->gradient_h_bohr = (double *)calloc((size_t)n_atoms * 3u, sizeof(double));
  parsed->atomic_numbers = (int *)calloc((size_t)n_atoms, sizeof(int));
  if (parsed->gradient_h_bohr == NULL || parsed->atomic_numbers == NULL) {
    orcac_free_engrad(parsed);
    orcac_errf(errbuf, errbuf_n, "orcac: malformed engrad: out of memory");
    return -1;
  }

  for (i = 0; i < n_atoms * 3; ++i) {
    if (read_double(&cur, &parsed->gradient_h_bohr[i]) != 0) {
      orcac_free_engrad(parsed);
      orcac_errf(errbuf, errbuf_n,
                 "orcac: malformed engrad: missing or bad gradient entry %d",
                 i);
      return -1;
    }
  }

  for (i = 0; i < n_atoms; ++i) {
    double xyz;
    if (read_int(&cur, &parsed->atomic_numbers[i]) != 0) {
      orcac_free_engrad(parsed);
      orcac_errf(errbuf, errbuf_n,
                 "orcac: malformed engrad: missing or bad atomic number %d",
                 i);
      return -1;
    }
    for (int k = 0; k < 3; ++k) {
      if (read_double(&cur, &xyz) != 0) {
        orcac_free_engrad(parsed);
        orcac_errf(errbuf, errbuf_n,
                   "orcac: malformed engrad: missing or bad coordinate of "
                   "atom %d",
                   i);
        return -1;
      }
    }
  }

  return 0;
}

/* tolower without the locale, so only A-Z change. */
static unsigned char ascii_lower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

/* Case-insensitive search for needle, given in lower case, in [line, end). */
static int line_has(const char *line, const char *end, const char *needle) {
  size_t n = strlen(needle);
  for (const char *p = line; (size_t)(end - p) >= n; ++p) {
    size_t i = 0;
    while (i < n &&
           ascii_lower((unsigned char)p[i]) == (unsigned char)needle[i]) {
      ++i;
    }
    if (i == n) {
      return 1;
    }
  }
  return 0;
}

/* 1 when [line, end) is word, in any case, with only blanks around it. */
static int line_is(const char *line, const char *end, const char *word) {
  while (line < end && isspace((unsigned char)*line)) {
    ++line;
  }
  while (end > line && isspace((unsigned char)end[-1])) {
    --end;
  }
  return (size_t)(end - line) == strlen(word) && line_has(line, end, word);
}

/* ORCA echoes each input line as "|  12> text". */
static int is_echo_line(const char *line, const char *end) {
  const char *p = line;
  while (p < end && *p == ' ') {
    ++p;
  }
  if (p == end || *p != '|') {
    return 0;
  }
  ++p;
  while (p < end && *p == ' ') {
    ++p;
  }
  if (p == end || !isdigit((unsigned char)*p)) {
    return 0;
  }
  while (p < end && isdigit((unsigned char)*p)) {
    ++p;
  }
  return p < end && *p == '>';
}

/* SCF failure banners of ORCA, counted on any line outside the echo. */
static const char *const kScfBanners[] = {
    "scf not converged after",
    "error termination in scf",
    "the scf has not converged",
    "scf did not converge",
    "this wavefunction is not converged",
};

/* Counted only inside the SCF iterations, before the SCF energy block. */
static const char *const kScfRegionPhrases[] = {
    "not yet converged",
    "not fully converged",
};

#define N_OF(a) (sizeof(a) / sizeof((a)[0]))

static int line_has_any(const char *line, const char *end,
                        const char *const *needles, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (line_has(line, end, needles[i])) {
      return 1;
    }
  }
  return 0;
}

/*
 * The input echo, from the "INPUT FILE" header to "****END OF INPUT****"
 * and every "|  n>" line, is skipped, so a comment in the deck cannot
 * read as a banner. Other "not converged" lines, such as a geometry
 * optimisation's, do not count.
 */
int orcac_scf_not_converged(const char *text) {
  const char *line = text;
  int in_input = 0;
  int in_scf = 0;
  if (text == NULL) {
    return 0;
  }
  while (*line != '\0') {
    const char *end = strchr(line, '\n');
    const char *next;
    if (end == NULL) {
      end = line + strlen(line);
    }
    next = *end == '\n' ? end + 1 : end;
    if (in_input) {
      if (line_has(line, end, "****end of input****")) {
        in_input = 0;
      }
      line = next;
      continue;
    }
    if (line_is(line, end, "input file")) {
      in_input = 1;
      line = next;
      continue;
    }
    if (is_echo_line(line, end)) {
      line = next;
      continue;
    }
    if (line_has_any(line, end, kScfBanners, N_OF(kScfBanners))) {
      return 1;
    }
    if (line_has(line, end, "scf iterations")) {
      in_scf = 1;
    } else if (line_has(line, end, "total scf energy")) {
      in_scf = 0;
    }
    if (in_scf &&
        line_has_any(line, end, kScfRegionPhrases, N_OF(kScfRegionPhrases))) {
      return 1;
    }
    line = next;
  }
  return 0;
}

int orcac_terminated_normally(const char *text) {
  return text != NULL && strstr(text, "ORCA TERMINATED NORMALLY") != NULL;
}
