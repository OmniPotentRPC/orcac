/* Shared helpers for the orcac C tests: Cap'n Proto message builders and a
 * minimal check macro. Header-only; each test is one translation unit. */
#pragma once

#if !defined(_GNU_SOURCE) && !defined(__APPLE__)
#define _GNU_SOURCE
#endif

#include "orcac.h"
#include "potential.capnp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define TEST_HELPER __attribute__((unused))
#else
#define TEST_HELPER
#endif

static int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n  last error: %s\n", __FILE__, \
              __LINE__, #cond, orcac_last_error());                            \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
  do {                                                                         \
    const double check_a_ = (a), check_b_ = (b);                               \
    if (!(fabs(check_a_ - check_b_) <= (tol))) {                               \
      fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %.15g, %s = %.15g\n",    \
              __FILE__, __LINE__, #a, check_a_, #b, check_b_);                 \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

TEST_HELPER static capn_text test_text(const char *s) {
  capn_text t = {s ? (int)strlen(s) : 0, s ? s : "", NULL};
  return t;
}

/* ORCAParams with the schema defaults. */
TEST_HELPER static struct ORCAParams test_params_defaults(void) {
  struct ORCAParams p;
  memset(&p, 0, sizeof(p));
  p.executable = test_text("");
  p.method = test_text("");
  p.basis = test_text("");
  p.keywords = test_text("");
  p.scratchDir = test_text("");
  p.multiplicity = 1;
  p.nprocs = 1;
  p.maxcoreMb = 1024;
  p.restart = 1;
  p.keepFiles = 0;
  p.timeoutS = 3600.0;
  return p;
}

/* Serialise the arena rooted at root into a malloc'd flat message. */
TEST_HELPER static uint8_t *test_finish(struct capn *arena, size_t *size) {
  uint8_t *buf = malloc(1 << 16);
  const int n = capn_write_mem(arena, buf, 1 << 16, 0);
  capn_free(arena);
  if (n < 0) {
    free(buf);
    *size = 0;
    return NULL;
  }
  *size = (size_t)n;
  return buf;
}

TEST_HELPER static uint8_t *test_encode_params(const struct ORCAParams *p, size_t *size) {
  struct capn arena;
  capn_init_malloc(&arena);
  capn_ptr root = capn_root(&arena);
  ORCAParams_ptr ptr = new_ORCAParams(root.seg);
  write_ORCAParams(p, ptr);
  capn_setp(root, 0, ptr.p);
  return test_finish(&arena, size);
}

/* PotentialConfig holding the orca arm and, when common is non-NULL, the
 * CommonMethodSpec overlay. xc may hold up to four names. */
TEST_HELPER static uint8_t *test_encode_config(const struct ORCAParams *p,
                                   const struct CommonMethodSpec *common,
                                   const char *const *xc, int n_xc,
                                   size_t *size) {
  struct capn arena;
  capn_init_malloc(&arena);
  capn_ptr root = capn_root(&arena);
  PotentialConfig_ptr cfg = new_PotentialConfig(root.seg);
  struct PotentialConfig view;
  memset(&view, 0, sizeof(view));
  view.which = PotentialConfig_orca;
  view.orca = new_ORCAParams(root.seg);
  write_ORCAParams(p, view.orca);
  view.emtRasmussenParameters = 1;
  if (common) {
    struct CommonMethodSpec c = *common;
    if (n_xc > 0) {
      c.xcFunctionals = capn_new_ptr_list(root.seg, n_xc);
      for (int i = 0; i < n_xc; ++i)
        capn_set_text(c.xcFunctionals, i, test_text(xc[i]));
    }
    view.common = new_CommonMethodSpec(root.seg);
    write_CommonMethodSpec(&c, view.common);
  }
  write_PotentialConfig(&view, cfg);
  capn_setp(root, 0, cfg.p);
  return test_finish(&arena, size);
}

TEST_HELPER static uint8_t *test_encode_spec(const struct ORCAParams *p, int orca_arm,
                                 size_t *size) {
  struct capn arena;
  capn_init_malloc(&arena);
  capn_ptr root = capn_root(&arena);
  PotentialSpec_ptr spec = new_PotentialSpec(root.seg);
  struct PotentialSpec view;
  memset(&view, 0, sizeof(view));
  if (orca_arm) {
    view.which = PotentialSpec_orca;
    view.orca = new_ORCAParams(root.seg);
    write_ORCAParams(p, view.orca);
  } else {
    view.which = PotentialSpec_builtin;
    view.builtin = new_BuiltinParams(root.seg);
  }
  write_PotentialSpec(&view, spec);
  capn_setp(root, 0, spec.p);
  return test_finish(&arena, size);
}

TEST_HELPER static struct CommonMethodSpec test_common_defaults(void) {
  struct CommonMethodSpec c;
  memset(&c, 0, sizeof(c));
  c.basisSet = test_text("");
  c.vanDerWaalsMethod = test_text("");
  c.relativityMethod = test_text("");
  return c;
}

TEST_HELPER static char *test_read_file(const char *path, size_t *length) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(2);
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fprintf(stderr, "cannot read %s\n", path);
    exit(2);
  }
  fclose(f);
  buf[n] = '\0';
  if (length)
    *length = (size_t)n;
  return buf;
}

TEST_HELPER static char *test_path(const char *dir, const char *name) {
  const size_t n = strlen(dir) + strlen(name) + 2;
  char *out = malloc(n);
  snprintf(out, n, "%s/%s", dir, name);
  return out;
}

TEST_HELPER static int test_finish_main(const char *name) {
  if (g_failures) {
    fprintf(stderr, "%s: %d check(s) failed\n", name, g_failures);
    return 1;
  }
  printf("%s: ok\n", name);
  return 0;
}
