#include "orca_input.h"
#include "orcac_errbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int has_line_break(const char *text) {
  if (text == NULL) {
    return 0;
  }
  return strchr(text, '\n') != NULL || strchr(text, '\r') != NULL;
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

int orcac_render_input(const OrcaCConfig *cfg, int n_atoms,
                       const double *positions_ang,
                       const int *atomic_numbers,
                       const char *moinp_name,
                       char **input_text,
                       char *errbuf,
                       size_t errbuf_n) {
  int i;
  int moread;
  TextBuf b = {NULL, 0u, 0u, 0};

  if (input_text == NULL || cfg == NULL || positions_ang == NULL ||
      atomic_numbers == NULL || n_atoms <= 0) {
    orcac_errf(errbuf, errbuf_n, "orcac: invalid input-writer arguments");
    return -1;
  }
  if (has_line_break(cfg->method) || has_line_break(cfg->basis) ||
      has_line_break(cfg->keywords) || has_line_break(moinp_name)) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: ORCA simple-input fields must be single-line");
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

  buf_appendf(&b, "! %s", cfg->method != NULL ? cfg->method : "HF");
  if (cfg->basis != NULL && cfg->basis[0] != '\0') {
    buf_appendf(&b, " %s", cfg->basis);
  }
  if (cfg->keywords != NULL && cfg->keywords[0] != '\0') {
    buf_appendf(&b, " %s", cfg->keywords);
  }
  buf_appendf(&b, " EnGrad");
  if (moread) {
    buf_appendf(&b, " MORead");
  }
  buf_appendf(&b, "\n");

  if (cfg->maxcore_mb > 0) {
    buf_appendf(&b, "%%maxcore %d\n", cfg->maxcore_mb);
  }
  if (cfg->nprocs > 1) {
    buf_appendf(&b, "%%pal nprocs %d end\n", cfg->nprocs);
  }
  if (moread) {
    buf_appendf(&b, "%%moinp \"%s\"\n", moinp_name);
  }

  buf_appendf(&b, "* xyz %d %d\n", cfg->charge,
              cfg->multiplicity > 0 ? cfg->multiplicity : 1);
  for (i = 0; i < n_atoms; ++i) {
    buf_appendf(&b, "%s  %.10f %.10f %.10f\n",
                orcac_element_symbol(atomic_numbers[i]),
                positions_ang[3 * i], positions_ang[3 * i + 1],
                positions_ang[3 * i + 2]);
  }
  buf_appendf(&b, "*\n");

  if (b.failed) {
    free(b.data);
    orcac_errf(errbuf, errbuf_n, "orcac: out of memory while writing input");
    return -1;
  }
  *input_text = b.data;
  return 0;
}
