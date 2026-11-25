/* ORCA input text, .engrad parsing and SCF-failure detection. No process
 * handling here, so all of it is testable without an ORCA installation. */
#if !defined(_GNU_SOURCE) && !defined(__APPLE__)
#define _GNU_SOURCE
#endif

#include "orcac_internal.h"

#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <xlocale.h>
#endif

static const char *const k_symbols[] = {
    "H",  "He", "Li", "Be", "B",  "C",  "N",  "O",  "F",  "Ne", "Na", "Mg",
    "Al", "Si", "P",  "S",  "Cl", "Ar", "K",  "Ca", "Sc", "Ti", "V",  "Cr",
    "Mn", "Fe", "Co", "Ni", "Cu", "Zn", "Ga", "Ge", "As", "Se", "Br", "Kr",
    "Rb", "Sr", "Y",  "Zr", "Nb", "Mo", "Tc", "Ru", "Rh", "Pd", "Ag", "Cd",
    "In", "Sn", "Sb", "Te", "I",  "Xe", "Cs", "Ba", "La", "Ce", "Pr", "Nd",
    "Pm", "Sm", "Eu", "Gd", "Tb", "Dy", "Ho", "Er", "Tm", "Yb", "Lu", "Hf",
    "Ta", "W",  "Re", "Os", "Ir", "Pt", "Au", "Hg", "Tl", "Pb", "Bi", "Po",
    "At", "Rn", "Fr", "Ra", "Ac", "Th", "Pa", "U",  "Np", "Pu", "Am", "Cm",
    "Bk", "Cf", "Es", "Fm", "Md", "No", "Lr", "Rf", "Db", "Sg", "Bh", "Hs",
    "Mt", "Ds", "Rg", "Cn", "Nh", "Fl", "Mc", "Lv", "Ts", "Og",
};

const char *orcac_element_symbol(int z) {
  const int n = (int)(sizeof(k_symbols) / sizeof(k_symbols[0]));
  if (z < 1 || z > n)
    return NULL;
  return k_symbols[z - 1];
}

/* ---- C-locale scope: numbers are written and read the same way whatever
 * LC_NUMERIC the host process set. */

typedef struct c_locale_scope {
  locale_t c_loc;
  locale_t previous;
} c_locale_scope;

static int c_locale_enter(c_locale_scope *scope) {
  scope->c_loc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (scope->c_loc == (locale_t)0)
    return -1;
  scope->previous = uselocale(scope->c_loc);
  return 0;
}

static void c_locale_leave(c_locale_scope *scope) {
  uselocale(scope->previous);
  freelocale(scope->c_loc);
}

/* ---- growable string ---- */

typedef struct sbuf {
  char *data;
  size_t len;
  size_t cap;
  int failed;
} sbuf;

static void sb_reserve(sbuf *sb, size_t extra) {
  if (sb->failed)
    return;
  if (sb->len + extra + 1 <= sb->cap)
    return;
  size_t cap = sb->cap ? sb->cap : 256;
  while (sb->len + extra + 1 > cap)
    cap *= 2;
  char *grown = realloc(sb->data, cap);
  if (!grown) {
    sb->failed = 1;
    return;
  }
  sb->data = grown;
  sb->cap = cap;
}

static void sb_append(sbuf *sb, const char *text, size_t n) {
  sb_reserve(sb, n);
  if (sb->failed)
    return;
  memcpy(sb->data + sb->len, text, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}

static void sb_puts(sbuf *sb, const char *text) {
  sb_append(sb, text, strlen(text));
}

static void sb_printf(sbuf *sb, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void sb_printf(sbuf *sb, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list copy;
  va_copy(copy, ap);
  const int need = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (need < 0) {
    sb->failed = 1;
    va_end(ap);
    return;
  }
  sb_reserve(sb, (size_t)need);
  if (!sb->failed) {
    vsnprintf(sb->data + sb->len, (size_t)need + 1, fmt, ap);
    sb->len += (size_t)need;
  }
  va_end(ap);
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

static void set_err(char *err, size_t errlen, const char *fmt, ...) {
  if (!err || errlen == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, errlen, fmt, ap);
  va_end(ap);
}

/* ---- keyword handling ---- */

static const char *skip_space(const char *s) {
  while (*s && isspace((unsigned char)*s))
    ++s;
  return s;
}

/* [begin, end) of s without surrounding white space. */
static void trimmed_span(const char *s, const char **begin, const char **end) {
  const char *b = skip_space(s);
  const char *e = b + strlen(b);
  while (e > b && isspace((unsigned char)e[-1]))
    --e;
  *begin = b;
  *end = e;
}

int orcac_keywords_are_block(const char *keywords) {
  if (!keywords)
    return 0;
  const char *b, *e;
  trimmed_span(keywords, &b, &e);
  if (b == e)
    return 0;
  if (*b == '!' || *b == '%')
    return 1;
  for (const char *p = b; p < e; ++p)
    if (*p == '\n' || *p == '\r')
      return 1;
  return 0;
}

static int has_line_break(const char *s) {
  return s && strpbrk(s, "\r\n") != NULL;
}

static int token_is_engrad(const char *b, const char *e) {
  static const char word[] = "engrad";
  if ((size_t)(e - b) != sizeof(word) - 1)
    return 0;
  for (size_t i = 0; i < sizeof(word) - 1; ++i)
    if (tolower((unsigned char)b[i]) != word[i])
      return 0;
  return 1;
}

/* Append every blank-separated token of text after a space. Sets *engrad
 * when one of them is EnGrad (ORCA keywords are case-insensitive). */
static void append_tokens(sbuf *sb, const char *text, int *engrad) {
  const char *p = text;
  for (;;) {
    p = skip_space(p);
    if (!*p)
      break;
    const char *b = p;
    while (*p && !isspace((unsigned char)*p))
      ++p;
    if (token_is_engrad(b, p))
      *engrad = 1;
    sb_puts(sb, " ");
    sb_append(sb, b, (size_t)(p - b));
  }
}

char *orcac_render(const orcac_config *cfg, int n_atoms, const double *pos_ang,
                   const int *atomic_numbers, const char *moinp, char *err,
                   size_t errlen) {
  if (!cfg) {
    set_err(err, errlen, "orcac: null configuration");
    return NULL;
  }
  if (n_atoms < 1 || !pos_ang || !atomic_numbers) {
    set_err(err, errlen, "orcac: empty geometry");
    return NULL;
  }
  if (orcac_config_validate(cfg, err, errlen) != 0)
    return NULL;
  if (has_line_break(moinp)) {
    set_err(err, errlen, "orcac: moinp must not contain a line break");
    return NULL;
  }
  for (int i = 0; i < n_atoms; ++i) {
    if (!orcac_element_symbol(atomic_numbers[i])) {
      set_err(err, errlen, "orcac: atomic number %d has no element symbol",
              atomic_numbers[i]);
      return NULL;
    }
    for (int k = 0; k < 3; ++k) {
      if (!isfinite(pos_ang[3 * i + k])) {
        set_err(err, errlen, "orcac: coordinate %d of atom %d is not finite",
                k, i);
        return NULL;
      }
    }
  }

  c_locale_scope scope;
  if (c_locale_enter(&scope) != 0) {
    set_err(err, errlen, "orcac: could not create the C locale");
    return NULL;
  }

  sbuf sb = {0};
  const int block = orcac_keywords_are_block(cfg->keywords);
  int engrad = 0;
  sb_puts(&sb, "!");
  append_tokens(&sb, cfg->method, &engrad);
  append_tokens(&sb, cfg->basis, &engrad);
  if (!block)
    append_tokens(&sb, cfg->keywords, &engrad);
  if (!engrad)
    sb_puts(&sb, " EnGrad");
  sb_puts(&sb, "\n");
  if (moinp && moinp[0]) {
    sb_puts(&sb, "! MORead\n");
    sb_printf(&sb, "%%moinp \"%s\"\n", moinp);
  }
  if (cfg->nprocs > 1)
    sb_printf(&sb, "%%pal\n  nprocs %d\nend\n", cfg->nprocs);
  if (cfg->maxcore_mb > 0)
    sb_printf(&sb, "%%maxcore %d\n", cfg->maxcore_mb);
  if (block) {
    const char *b, *e;
    trimmed_span(cfg->keywords, &b, &e);
    sb_append(&sb, b, (size_t)(e - b));
    sb_puts(&sb, "\n");
  }
  sb_printf(&sb, "* xyz %d %d\n", cfg->charge, cfg->multiplicity);
  for (int i = 0; i < n_atoms; ++i) {
    sb_printf(&sb, "%-2s %18.10f %18.10f %18.10f\n",
              orcac_element_symbol(atomic_numbers[i]), pos_ang[3 * i],
              pos_ang[3 * i + 1], pos_ang[3 * i + 2]);
  }
  sb_puts(&sb, "*\n");
  c_locale_leave(&scope);

  if (sb.failed) {
    free(sb.data);
    set_err(err, errlen, "orcac: out of memory rendering the input");
    return NULL;
  }
  return sb.data;
}

/* ---- .engrad ---- */

enum engrad_section {
  SEC_NONE,
  SEC_NATOMS,
  SEC_ENERGY,
  SEC_GRADIENT,
  SEC_COORDS,
  SEC_OTHER,
};

static int contains_ci(const char *b, const char *e, const char *needle) {
  const size_t n = strlen(needle);
  for (const char *p = b; p + n <= e; ++p) {
    size_t i = 0;
    while (i < n && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
      ++i;
    if (i == n)
      return 1;
  }
  return 0;
}

/* Section named by a comment line, or -1 for a bare '#' that keeps the
 * current one. */
static int section_of(const char *b, const char *e) {
  while (b < e && (*b == '#' || isspace((unsigned char)*b)))
    ++b;
  if (b == e)
    return -1;
  if (contains_ci(b, e, "number of atoms"))
    return SEC_NATOMS;
  if (contains_ci(b, e, "total energy"))
    return SEC_ENERGY;
  if (contains_ci(b, e, "gradient"))
    return SEC_GRADIENT;
  if (contains_ci(b, e, "atomic numbers"))
    return SEC_COORDS;
  return SEC_OTHER;
}

/* Copies the next token of [*p, e) into tok (NUL-terminated). 0 at end. */
static int next_token(const char **p, const char *e, char *tok, size_t cap) {
  const char *s = *p;
  while (s < e && isspace((unsigned char)*s))
    ++s;
  if (s == e) {
    *p = s;
    return 0;
  }
  const char *b = s;
  while (s < e && !isspace((unsigned char)*s))
    ++s;
  size_t n = (size_t)(s - b);
  if (n >= cap)
    n = cap - 1;
  memcpy(tok, b, n);
  tok[n] = '\0';
  *p = s;
  return 1;
}

static int parse_real(const char *tok, double *out) {
  char *end = NULL;
  errno = 0;
  const double v = strtod(tok, &end);
  if (end == tok || *end != '\0')
    return -1;
  *out = v;
  return 0;
}

static int parse_long(const char *tok, long *out) {
  char *end = NULL;
  errno = 0;
  const long v = strtol(tok, &end, 10);
  if (end == tok || *end != '\0' || errno == ERANGE)
    return -1;
  *out = v;
  return 0;
}

static int engrad_parse_locked(const char *text, size_t length, int n_atoms,
                               double *energy_h, double *grad_h_bohr,
                               int *atomic_numbers, char *err, size_t errlen) {
  int section = SEC_NONE;
  int saw_natoms = 0, saw_energy = 0;
  long declared = 0;
  double energy = 0.0;
  size_t ngrad = 0;
  int ncoord = 0;
  const size_t need = 3u * (size_t)n_atoms;
  char tok[128];

  const char *p = text;
  const char *end = text + length;
  while (p < end) {
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    if (!eol)
      eol = end;
    const char *b = p, *e = eol;
    while (b < e && isspace((unsigned char)*b))
      ++b;
    while (e > b && isspace((unsigned char)e[-1]))
      --e;
    p = eol < end ? eol + 1 : end;
    if (b == e)
      continue;
    if (*b == '#') {
      const int next = section_of(b, e);
      if (next >= 0)
        section = next;
      continue;
    }
    const char *q = b;
    switch (section) {
    case SEC_NATOMS:
      if (!saw_natoms) {
        if (!next_token(&q, e, tok, sizeof(tok)) || parse_long(tok, &declared) ||
            declared < 1) {
          set_err(err, errlen,
                  "orcac: malformed .engrad: the atom count is not a positive "
                  "integer");
          return -1;
        }
        if (declared != n_atoms) {
          set_err(err, errlen,
                  "orcac: malformed .engrad: it holds %ld atoms, the input had "
                  "%d",
                  declared, n_atoms);
          return -1;
        }
        saw_natoms = 1;
      }
      break;
    case SEC_ENERGY:
      if (!saw_energy) {
        if (!next_token(&q, e, tok, sizeof(tok)) || parse_real(tok, &energy)) {
          set_err(err, errlen, "orcac: malformed .engrad: the energy is not a "
                               "number");
          return -1;
        }
        saw_energy = 1;
      }
      break;
    case SEC_GRADIENT:
      while (next_token(&q, e, tok, sizeof(tok))) {
        double v = 0.0;
        if (parse_real(tok, &v)) {
          set_err(err, errlen,
                  "orcac: malformed .engrad: gradient component '%s' is not a "
                  "number",
                  tok);
          return -1;
        }
        if (ngrad < need)
          grad_h_bohr[ngrad] = v;
        ++ngrad;
      }
      break;
    case SEC_COORDS: {
      long z = 0;
      if (!next_token(&q, e, tok, sizeof(tok)) || parse_long(tok, &z)) {
        set_err(err, errlen,
                "orcac: malformed .engrad: a coordinate line does not start "
                "with an atomic number");
        return -1;
      }
      if (ncoord < n_atoms && atomic_numbers)
        atomic_numbers[ncoord] = (int)z;
      ++ncoord;
      break;
    }
    default:
      break;
    }
  }

  if (!saw_natoms) {
    set_err(err, errlen, "orcac: malformed .engrad: no atom count");
    return -1;
  }
  if (!saw_energy) {
    set_err(err, errlen, "orcac: malformed .engrad: no energy");
    return -1;
  }
  if (ngrad != need) {
    set_err(err, errlen,
            "orcac: malformed .engrad: expected %zu gradient components, "
            "found %zu",
            need, ngrad);
    return -1;
  }
  if (ncoord != n_atoms) {
    set_err(err, errlen,
            "orcac: malformed .engrad: expected %d coordinate lines, found %d",
            n_atoms, ncoord);
    return -1;
  }
  if (!isfinite(energy)) {
    set_err(err, errlen, "orcac: non-finite energy in the .engrad file");
    return -1;
  }
  for (size_t k = 0; k < need; ++k) {
    if (!isfinite(grad_h_bohr[k])) {
      set_err(err, errlen, "orcac: non-finite gradient in the .engrad file");
      return -1;
    }
  }
  *energy_h = energy;
  return 0;
}

int orcac_engrad_parse(const char *text, size_t length, int n_atoms,
                       double *energy_h, double *grad_h_bohr,
                       int *atomic_numbers, char *err, size_t errlen) {
  if (!text || n_atoms < 1 || !energy_h || !grad_h_bohr) {
    set_err(err, errlen, "orcac: invalid argument to the .engrad parser");
    return -1;
  }
  c_locale_scope scope;
  if (c_locale_enter(&scope) != 0) {
    set_err(err, errlen, "orcac: could not create the C locale");
    return -1;
  }
  const int rc = engrad_parse_locked(text, length, n_atoms, energy_h,
                                     grad_h_bohr, atomic_numbers, err, errlen);
  c_locale_leave(&scope);
  return rc;
}

/* ---- SCF failure ---- */

static int contains(const char *text, size_t length, const char *needle) {
  const size_t n = strlen(needle);
  if (n == 0 || length < n)
    return 0;
  for (size_t i = 0; i + n <= length; ++i)
    if (text[i] == needle[0] && memcmp(text + i, needle, n) == 0)
      return 1;
  return 0;
}

int orcac_scf_failed(const char *text, size_t length) {
  /* Wordings ORCA prints when the SCF stops unconverged, and the
   * error-termination line that follows them. */
  static const char *const markers[] = {
      "SCF NOT CONVERGED",
      "SCF DID NOT CONVERGE",
      "The SCF has not converged",
      "error termination in SCF",
  };
  if (!text)
    return 0;
  for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i)
    if (contains(text, length, markers[i]))
      return 1;
  return 0;
}
