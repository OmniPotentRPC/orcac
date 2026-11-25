#include "orca_parse.h"
#include "orcac_clocale.h"
#include "orcac_errbuf.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

/*
 * Open name relative to dirfd for reading, refusing a final symlink and
 * anything but a regular file. O_NONBLOCK keeps a FIFO planted under the
 * name from blocking the open; it does nothing for a regular file.
 */
static int open_regular_at(int dirfd, const char *name, struct stat *st) {
  int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  if (fstat(fd, st) != 0 || !S_ISREG(st->st_mode)) {
    close(fd);
    errno = EINVAL;
    return -1;
  }
  return fd;
}

/* Up to n bytes from fd into buf; returns the count read. */
static size_t read_up_to(int fd, char *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, buf + got, n - got);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r <= 0) {
      break;
    }
    got += (size_t)r;
  }
  return got;
}

int orcac_read_file_at(int dirfd, const char *name, char **text_out) {
  struct stat st;
  char *text;
  int fd;
  if (name == NULL || text_out == NULL) {
    return -1;
  }
  fd = open_regular_at(dirfd, name, &st);
  if (fd < 0) {
    return -1;
  }
  text = (char *)malloc((size_t)st.st_size + 1u);
  if (text == NULL) {
    close(fd);
    return -1;
  }
  text[read_up_to(fd, text, (size_t)st.st_size)] = '\0';
  close(fd);
  *text_out = text;
  return 0;
}

int orcac_read_file(const char *path, char **text_out) {
  return orcac_read_file_at(AT_FDCWD, path, text_out);
}

char *orcac_file_tail_at(int dirfd, const char *name, size_t max_bytes) {
  struct stat st;
  off_t keep;
  char *text;
  int fd;
  if (name == NULL) {
    return dup_text("(no path)");
  }
  fd = open_regular_at(dirfd, name, &st);
  if (fd < 0) {
    return dup_text("(missing)");
  }
  if (st.st_size <= 0) {
    close(fd);
    return dup_text("(empty)");
  }
  keep = (off_t)max_bytes;
  if (keep <= 0 || keep > st.st_size) {
    keep = st.st_size;
  }
  if (lseek(fd, st.st_size - keep, SEEK_SET) < 0) {
    close(fd);
    return dup_text("(unreadable)");
  }
  text = (char *)malloc((size_t)keep + 1u);
  if (text == NULL) {
    close(fd);
    return dup_text("(oom)");
  }
  text[read_up_to(fd, text, (size_t)keep)] = '\0';
  close(fd);
  return text;
}

/* isspace and isdigit without the locale. */
static int ascii_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

static int ascii_digit(unsigned char c) {
  return c >= '0' && c <= '9';
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
    if (!ascii_space((unsigned char)*p)) {
      break;
    }
    ++p;
  }
  if (*p == '\0') {
    *cursor = p;
    return 0;
  }
  while (*p != '\0' && !ascii_space((unsigned char)*p) && *p != '#') {
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
 * returns, which is zero or subnormal. Callers hold the C numeric
 * locale, so '.' is the decimal point whatever the host set.
 */
static int parse_double(const char *token, double *out) {
  char *end = NULL;
  double v;
  for (const char *p = token; *p != '\0'; ++p) {
    if (!ascii_digit((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.' &&
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

static int parse_engrad(const char *text, int expected_atoms,
                        OrcacEngrad *parsed, char *errbuf, size_t errbuf_n) {
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

int orcac_parse_engrad(const char *text,
                       int expected_atoms,
                       OrcacEngrad *parsed,
                       char *errbuf,
                       size_t errbuf_n) {
  OrcacCNumeric c_numeric;
  int rc;
  if (text == NULL || parsed == NULL || expected_atoms <= 0) {
    return -1;
  }
  if (orcac_c_numeric_enter(&c_numeric) != 0) {
    memset(parsed, 0, sizeof(*parsed));
    orcac_errf(errbuf, errbuf_n,
               "orcac: could not switch to the C numeric locale");
    return -1;
  }
  rc = parse_engrad(text, expected_atoms, parsed, errbuf, errbuf_n);
  orcac_c_numeric_leave(&c_numeric);
  return rc;
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

/* isspace without the locale; '\n' never occurs inside a line. */
static int is_blank(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

/* 1 when [line, end) is word, in any case, with only blanks around it. */
static int line_is(const char *line, const char *end, const char *word) {
  while (line < end && is_blank(*line)) {
    ++line;
  }
  while (end > line && is_blank(end[-1])) {
    --end;
  }
  return (size_t)(end - line) == strlen(word) && line_has(line, end, word);
}

/*
 * 1 for a line that may sit inside the echo block between its header and
 * the end marker without being an echoed deck line: blank, a rule of
 * '=', or ORCA's "NAME = file" line.
 */
static int is_echo_frame_line(const char *line, const char *end) {
  const char *p = line;
  while (p < end && is_blank(*p)) {
    ++p;
  }
  if (p < end && *p == '=') {
    while (p < end && (*p == '=' || is_blank(*p))) {
      ++p;
    }
    return p == end;
  }
  return p == end ||
         ((size_t)(end - p) >= 4u && memcmp(p, "NAME", 4u) == 0 &&
          line_has(p + 4, end, "="));
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
  if (p == end || !ascii_digit((unsigned char)*p)) {
    return 0;
  }
  while (p < end && ascii_digit((unsigned char)*p)) {
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
 * Walks ORCA output by line, skipping the input echo: every "|  n>" line,
 * and the block from the "INPUT FILE" header to "****END OF INPUT****".
 * Inside the block only echoed lines, blanks, '=' rules and the NAME line
 * are skipped; any other line ends the block early, sets echo_unclosed
 * and is returned, so a block without its end marker cannot hide the
 * rest of the output. echo_seen counts the skipped lines.
 */
typedef struct OutputLines {
  const char *at;
  int in_input;
  int echo_unclosed;
  unsigned long echo_seen;
} OutputLines;

static int next_output_line(OutputLines *it, const char **line_out,
                            const char **end_out) {
  while (*it->at != '\0') {
    const char *line = it->at;
    const char *end = strchr(line, '\n');
    if (end == NULL) {
      end = line + strlen(line);
    }
    it->at = *end == '\n' ? end + 1 : end;
    if (it->in_input) {
      if (line_has(line, end, "****end of input****")) {
        it->in_input = 0;
        ++it->echo_seen;
        continue;
      }
      if (is_echo_line(line, end) || is_echo_frame_line(line, end)) {
        ++it->echo_seen;
        continue;
      }
      it->in_input = 0;
      it->echo_unclosed = 1;
    }
    if (line_is(line, end, "input file")) {
      it->in_input = 1;
      ++it->echo_seen;
      continue;
    }
    if (is_echo_line(line, end)) {
      ++it->echo_seen;
      continue;
    }
    *line_out = line;
    *end_out = end;
    return 1;
  }
  if (it->in_input) {
    it->in_input = 0;
    it->echo_unclosed = 1;
  }
  return 0;
}

/*
 * A comment in the echoed deck cannot read as a banner. Other "not
 * converged" lines, such as a geometry optimisation's, do not count.
 */
int orcac_scf_not_converged(const char *text) {
  OutputLines it;
  const char *line;
  const char *end;
  int in_scf = 0;
  if (text == NULL) {
    return 0;
  }
  memset(&it, 0, sizeof(it));
  it.at = text;
  while (next_output_line(&it, &line, &end)) {
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
  }
  return 0;
}

/*
 * 1 when [line, end) is ORCA's banner, "****ORCA TERMINATED NORMALLY****",
 * with any number of asterisks, including none, and blanks around it.
 */
static int is_termination_banner(const char *line, const char *end) {
  static const char kBanner[] = "ORCA TERMINATED NORMALLY";
  while (line < end && (is_blank(*line) || *line == '*')) {
    ++line;
  }
  while (end > line && (is_blank(end[-1]) || end[-1] == '*')) {
    --end;
  }
  return (size_t)(end - line) == sizeof(kBanner) - 1u &&
         memcmp(line, kBanner, sizeof(kBanner) - 1u) == 0;
}

/*
 * The banner counts only as a line of its own, read by the same loop as
 * the SCF scan so the echoed deck is skipped, with no echo after it and
 * no echo block left without its end marker. Neither the echoed deck,
 * nor text before the echo, nor a line that cut an echo block short can
 * supply it.
 */
int orcac_terminated_normally(const char *text) {
  OutputLines it;
  const char *line;
  const char *end;
  int found = 0;
  unsigned long found_at = 0;
  if (text == NULL) {
    return 0;
  }
  memset(&it, 0, sizeof(it));
  it.at = text;
  while (next_output_line(&it, &line, &end)) {
    if (is_termination_banner(line, end)) {
      found = 1;
      found_at = it.echo_seen;
    }
  }
  return found && !it.echo_unclosed && found_at == it.echo_seen;
}
