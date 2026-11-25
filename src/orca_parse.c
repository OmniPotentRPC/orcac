#include "orca_parse.h"
#include "orcac_errbuf.h"

#include <ctype.h>
#include <errno.h>
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
    if (n + 1u < token_n) {
      token[n++] = *p;
    }
    ++p;
  }
  token[n] = '\0';
  *cursor = p;
  return 1;
}

int orcac_parse_engrad(const char *text,
                       int expected_atoms,
                       OrcacEngrad *parsed,
                       char *errbuf,
                       size_t errbuf_n) {
  const char *cur = text;
  char token[128];
  int n_atoms;
  int i;
  if (text == NULL || parsed == NULL || expected_atoms <= 0) {
    return -1;
  }
  memset(parsed, 0, sizeof(*parsed));

  if (!next_token(&cur, token, sizeof(token))) {
    orcac_errf(errbuf, errbuf_n, "orcac: malformed engrad: missing atom count");
    return -1;
  }
  n_atoms = (int)strtol(token, NULL, 10);
  if (n_atoms != expected_atoms) {
    orcac_errf(errbuf, errbuf_n,
               "orcac: malformed engrad: atom count %d does not match %d",
               n_atoms, expected_atoms);
    return -1;
  }
  parsed->n_atoms = n_atoms;

  if (!next_token(&cur, token, sizeof(token))) {
    orcac_errf(errbuf, errbuf_n, "orcac: malformed engrad: missing energy");
    return -1;
  }
  parsed->energy_h = strtod(token, NULL);

  parsed->gradient_h_bohr = (double *)calloc((size_t)n_atoms * 3u, sizeof(double));
  parsed->atomic_numbers = (int *)calloc((size_t)n_atoms, sizeof(int));
  if (parsed->gradient_h_bohr == NULL || parsed->atomic_numbers == NULL) {
    orcac_free_engrad(parsed);
    orcac_errf(errbuf, errbuf_n, "orcac: malformed engrad: out of memory");
    return -1;
  }

  for (i = 0; i < n_atoms * 3; ++i) {
    if (!next_token(&cur, token, sizeof(token))) {
      orcac_free_engrad(parsed);
      orcac_errf(errbuf, errbuf_n,
                 "orcac: malformed engrad: gradient length mismatch");
      return -1;
    }
    parsed->gradient_h_bohr[i] = strtod(token, NULL);
  }

  for (i = 0; i < n_atoms; ++i) {
    if (!next_token(&cur, token, sizeof(token))) {
      orcac_free_engrad(parsed);
      orcac_errf(errbuf, errbuf_n,
                 "orcac: malformed engrad: missing atomic numbers");
      return -1;
    }
    parsed->atomic_numbers[i] = (int)strtol(token, NULL, 10);
    if (!next_token(&cur, token, sizeof(token)) ||
        !next_token(&cur, token, sizeof(token)) ||
        !next_token(&cur, token, sizeof(token))) {
      orcac_free_engrad(parsed);
      orcac_errf(errbuf, errbuf_n,
                 "orcac: malformed engrad: missing coordinates");
      return -1;
    }
  }

  return 0;
}

int orcac_scf_not_converged(const char *text) {
  static const char *const markers[] = {
      "SCF NOT CONVERGED",
      "SCF DID NOT CONVERGE",
      "The SCF has not converged",
      "error termination in SCF",
  };
  if (text == NULL) {
    return 0;
  }
  for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
    if (strstr(text, markers[i]) != NULL) {
      return 1;
    }
  }
  return 0;
}
