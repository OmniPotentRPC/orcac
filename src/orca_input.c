#include "orca_input.h"
#include "orcac_clocale.h"
#include "orcac_errbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* isdigit and isalnum without the locale. */
static int ascii_digit(unsigned char c) {
  return c >= '0' && c <= '9';
}

static int ascii_alnum(unsigned char c) {
  return ascii_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static const char *kSymbols[] = {
    NULL, "H", "He", "Li", "Be", "B", "C", "N", "O", "F", "Ne", "Na",
    "Mg", "Al", "Si", "P", "S", "Cl", "Ar", "K", "Ca", "Sc", "Ti", "V",
    "Cr", "Mn", "Fe", "Co", "Ni", "Cu", "Zn", "Ga", "Ge", "As", "Se", "Br",
    "Kr", "Rb", "Sr", "Y", "Zr", "Nb", "Mo", "Tc", "Ru", "Rh", "Pd", "Ag",
    "Cd", "In", "Sn", "Sb", "Te", "I", "Xe", "Cs", "Ba", "La", "Ce", "Pr",
    "Nd", "Pm", "Sm", "Eu", "Gd", "Tb", "Dy", "Ho", "Er", "Tm", "Yb", "Lu",
    "Hf", "Ta", "W", "Re", "Os", "Ir", "Pt", "Au", "Hg", "Tl", "Pb", "Bi",
    "Po", "At", "Rn", "Fr", "Ra", "Ac", "Th", "Pa", "U", "Np", "Pu", "Am",
    "Cm", "Bk", "Cf", "Es", "Fm", "Md", "No", "Lr", "Rf", "Db", "Sg", "Bh",
    "Hs", "Mt", "Ds", "Rg", "Cn", "Nh", "Fl", "Mc", "Lv", "Ts", "Og",
};

const char *orcac_element_symbol(int atomic_number) {
  size_t limit = sizeof(kSymbols) / sizeof(kSymbols[0]);
  if (atomic_number < 1 || (size_t)atomic_number >= limit) {
    return NULL;
  }
  return kSymbols[atomic_number];
}

static int ascii_lower(int c) {
  return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Case-insensitive substring search over ASCII. */
static const char *find_nocase(const char *text, const char *needle) {
  size_t n = strlen(needle);
  for (const char *p = text; *p != '\0'; ++p) {
    size_t i = 0;
    while (i < n && p[i] != '\0' &&
           ascii_lower((unsigned char)p[i]) == ascii_lower((unsigned char)needle[i])) {
      ++i;
    }
    if (i == n) {
      return p;
    }
  }
  return NULL;
}

static int equal_nocase_n(const char *a, size_t na, const char *b) {
  size_t nb = strlen(b);
  if (na != nb) {
    return 0;
  }
  for (size_t i = 0; i < na; ++i) {
    if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i])) {
      return 0;
    }
  }
  return 1;
}

int orcac_validate_line(const char *what, const char *text, char *errbuf,
                        size_t errbuf_n) {
  if (text == NULL) {
    return 0;
  }
  for (const char *p = text; *p != '\0'; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c == '\n' || c == '\r') {
      orcac_errf(errbuf, errbuf_n, "orcac: %s must be a single line", what);
      return -1;
    }
    if ((c < 0x20 && c != '\t') || c == 0x7f) {
      orcac_errf(errbuf, errbuf_n,
                 "orcac: %s contains control character 0x%02x", what, c);
      return -1;
    }
  }
  return 0;
}

/* MORead and PAL<n> are orcac's; reject them as whole tokens. */
static int owned_token(const char *what, const char *start, size_t n,
                       char *errbuf, size_t errbuf_n) {
  if (equal_nocase_n(start, n, "moread")) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: %s may not contain MORead; use guess_policy", what);
    return -1;
  }
  if (n > 3 && equal_nocase_n(start, 3, "pal")) {
    size_t i = 3;
    while (i < n && ascii_digit((unsigned char)start[i])) {
      ++i;
    }
    if (i == n) {
      orcac_errf(errbuf, errbuf_n,
                 "orcac: %s may not contain %.*s; use nprocs", what, (int)n,
                 start);
      return -1;
    }
  }
  return 0;
}

int orcac_validate_keywords(const char *keywords, char *errbuf,
                            size_t errbuf_n) {
  const char *p;
  if (keywords == NULL) {
    return 0;
  }
  if (orcac_validate_line("keywords", keywords, errbuf, errbuf_n) != 0) {
    return -1;
  }
  if (strchr(keywords, '%') != NULL) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: keywords may not contain '%%'; use blocks");
    return -1;
  }
  if (find_nocase(keywords, "convforced") != NULL) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: keywords may not set ConvForced; orcac always forces "
               "SCF convergence");
    return -1;
  }
  if (strchr(keywords, '#') != NULL) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: keywords may not contain '#'; it would comment out "
               "the rest of the line");
    return -1;
  }
  p = keywords;
  while (*p != '\0') {
    const char *start;
    size_t n;
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
      ++p;
    }
    n = (size_t)(p - start);
    if (n != 0 && owned_token("keywords", start, n, errbuf, errbuf_n) != 0) {
      return -1;
    }
  }
  return 0;
}

int orcac_validate_token(const char *what, const char *text, char *errbuf,
                         size_t errbuf_n) {
  size_t n;
  if (text == NULL) {
    return 0;
  }
  for (const char *p = text; *p != '\0'; ++p) {
    unsigned char c = (unsigned char)*p;
    if (!ascii_alnum(c) && strchr("-+*(),/_.'", (int)c) == NULL) {
      if (c == '\n' || c == '\r') {
        orcac_errf(errbuf, errbuf_n, "orcac: %s must be a single line", what);
      } else if (c < 0x20 || c == 0x7f) {
        orcac_errf(errbuf, errbuf_n,
                   "orcac: %s contains control character 0x%02x", what, c);
      } else if (c == ' ') {
        orcac_errf(errbuf, errbuf_n,
                   "orcac: %s must be a single token without whitespace; "
                   "put extra keywords in keywords",
                   what);
      } else {
        orcac_errf(errbuf, errbuf_n, "orcac: %s contains '%c'", what, c);
      }
      return -1;
    }
  }
  n = strlen(text);
  if (n != 0 && find_nocase(text, "convforced") != NULL) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: %s may not set ConvForced; orcac always forces SCF "
               "convergence",
               what);
    return -1;
  }
  return n == 0 ? 0 : owned_token(what, text, n, errbuf, errbuf_n);
}

int orcac_validate_blocks(const char *blocks, char *errbuf, size_t errbuf_n) {
  static const char *const owned[] = {"pal",    "maxcore", "moinp",
                                      "output", "base",    "coords"};
  int line_start = 1;
  if (blocks == NULL) {
    return 0;
  }
  if (find_nocase(blocks, "convforced") != NULL) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: blocks may not set ConvForced; orcac always forces "
               "SCF convergence");
    return -1;
  }
  if (find_nocase(blocks, "moread") != NULL) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: blocks may not request MORead; use guess_policy");
    return -1;
  }
  if (find_nocase(blocks, "$new_job") != NULL) {
    orcac_errf(errbuf, errbuf_n, "orcac: blocks may not start a new job");
    return -1;
  }
  for (const char *p = blocks; *p != '\0'; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c == '\n') {
      line_start = 1;
      continue;
    }
    if ((c < 0x20 && c != '\t') || c == 0x7f) {
      orcac_errf(errbuf, errbuf_n,
                 "orcac: blocks contain control character 0x%02x", c);
      return -1;
    }
    if (line_start && (c == ' ' || c == '\t')) {
      continue;
    }
    if (line_start && (c == '!' || c == '*')) {
      orcac_errf(errbuf, errbuf_n,
                 c == '!' ? "orcac: blocks may not contain '!' lines; use "
                            "keywords"
                          : "orcac: blocks may not contain '*' geometry lines");
      return -1;
    }
    line_start = 0;
    if (c == '%') {
      const char *name = p + 1;
      size_t n = 0;
      while (*name == ' ' || *name == '\t') {
        ++name;
      }
      while (ascii_alnum((unsigned char)name[n]) || name[n] == '_') {
        ++n;
      }
      for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); ++i) {
        if (equal_nocase_n(name, n, owned[i])) {
          orcac_errf(errbuf, errbuf_n,
                     "orcac: blocks may not contain %%%s; orcac writes it",
                     owned[i]);
          return -1;
        }
      }
    }
  }
  return 0;
}

typedef struct TextBuf {
  char *data;
  size_t used;
  size_t cap;
  int failed;
} TextBuf;

static void buf_appendf(TextBuf *b, const char *fmt, ...) {
  va_list ap;
  int need;
  if (b->failed) {
    return;
  }
  va_start(ap, fmt);
  need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) {
    b->failed = 1;
    return;
  }
  if (b->used + (size_t)need + 1u > b->cap) {
    size_t cap = b->cap ? b->cap : 256u;
    char *grown;
    while (b->used + (size_t)need + 1u > cap) {
      cap *= 2u;
    }
    grown = (char *)realloc(b->data, cap);
    if (grown == NULL) {
      b->failed = 1;
      return;
    }
    b->data = grown;
    b->cap = cap;
  }
  va_start(ap, fmt);
  vsnprintf(b->data + b->used, b->cap - b->used, fmt, ap);
  va_end(ap);
  b->used += (size_t)need;
}

int orcac_render_input(const OrcacDeck *deck, int n_atoms,
                       const double *positions_ang,
                       const int *atomic_numbers,
                       const char *moinp_name,
                       char **input_text,
                       char *errbuf,
                       size_t errbuf_n) {
  int i;
  int moread;
  OrcacCNumeric c_numeric;
  TextBuf b = {NULL, 0u, 0u, 0};

  if (input_text == NULL || deck == NULL || positions_ang == NULL ||
      atomic_numbers == NULL || n_atoms <= 0) {
    orcac_errf(errbuf, errbuf_n, "orcac: invalid input-writer arguments");
    return -1;
  }
  if (orcac_validate_token("method", deck->method, errbuf, errbuf_n) != 0 ||
      orcac_validate_token("basis", deck->basis, errbuf, errbuf_n) != 0 ||
      orcac_validate_keywords(deck->keywords, errbuf, errbuf_n) != 0 ||
      orcac_validate_blocks(deck->blocks, errbuf, errbuf_n) != 0 ||
      orcac_validate_line("guess file", moinp_name, errbuf, errbuf_n) != 0) {
    return -1;
  }
  for (i = 0; i < n_atoms; ++i) {
    if (orcac_element_symbol(atomic_numbers[i]) == NULL) {
      orcac_errf(errbuf, errbuf_n, "orcac: unknown atomic number %d",
                 atomic_numbers[i]);
      return -1;
    }
  }

  moread = (moinp_name != NULL && moinp_name[0] != '\0');

  buf_appendf(&b, "! %s", deck->method != NULL ? deck->method : "HF");
  if (deck->basis != NULL && deck->basis[0] != '\0') {
    buf_appendf(&b, " %s", deck->basis);
  }
  if (deck->keywords != NULL && deck->keywords[0] != '\0') {
    buf_appendf(&b, " %s", deck->keywords);
  }
  buf_appendf(&b, " EnGrad");
  if (moread) {
    buf_appendf(&b, " MORead");
  }
  buf_appendf(&b, "\n");

  if (deck->blocks != NULL && deck->blocks[0] != '\0') {
    size_t n = strlen(deck->blocks);
    buf_appendf(&b, "%s%s", deck->blocks,
                deck->blocks[n - 1u] == '\n' ? "" : "\n");
  }
  if (deck->maxcore_mb > 0) {
    buf_appendf(&b, "%%maxcore %d\n", deck->maxcore_mb);
  }
  if (deck->nprocs > 1) {
    buf_appendf(&b, "%%pal nprocs %d end\n", deck->nprocs);
  }
  buf_appendf(&b, "%%scf ConvForced true end\n");
  if (moread) {
    buf_appendf(&b, "%%moinp \"%s\"\n", moinp_name);
  }

  buf_appendf(&b, "* xyz %d %d\n", deck->charge,
              deck->multiplicity > 0 ? deck->multiplicity : 1);
  if (orcac_c_numeric_enter(&c_numeric) != 0) {
    free(b.data);
    orcac_errf(errbuf, errbuf_n,
               "orcac: could not switch to the C numeric locale");
    return -1;
  }
  for (i = 0; i < n_atoms; ++i) {
    buf_appendf(&b, "%s  %.10f %.10f %.10f\n",
                orcac_element_symbol(atomic_numbers[i]),
                positions_ang[3 * i], positions_ang[3 * i + 1],
                positions_ang[3 * i + 2]);
  }
  orcac_c_numeric_leave(&c_numeric);
  buf_appendf(&b, "*\n");

  if (b.failed) {
    free(b.data);
    orcac_errf(errbuf, errbuf_n, "orcac: out of memory while writing input");
    return -1;
  }
  *input_text = b.data;
  return 0;
}
