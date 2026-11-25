/* ORCAParams / PotentialConfig / PotentialSpec decoding and the config
 * value type the rest of orcac works on. */
#if !defined(_GNU_SOURCE) && !defined(__APPLE__)
#define _GNU_SOURCE
#endif

#include "orcac_internal.h"

#include "potential.capnp.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int orcac_config_set_text(char **slot, const char *text, size_t length) {
  char *copy = malloc(length + 1);
  if (!copy)
    return -1;
  if (length)
    memcpy(copy, text, length);
  copy[length] = '\0';
  free(*slot);
  *slot = copy;
  return 0;
}

static int set_cstr(char **slot, const char *text) {
  return orcac_config_set_text(slot, text ? text : "", text ? strlen(text) : 0);
}

int orcac_config_init(orcac_config *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->multiplicity = 1;
  cfg->nprocs = 1;
  cfg->maxcore_mb = 1024;
  cfg->restart = 1;
  cfg->keep_files = 0;
  cfg->timeout_s = 3600.0;
  if (set_cstr(&cfg->executable, "") || set_cstr(&cfg->method, "") ||
      set_cstr(&cfg->basis, "") || set_cstr(&cfg->keywords, "") ||
      set_cstr(&cfg->scratch_dir, "")) {
    orcac_config_free(cfg);
    return -1;
  }
  return 0;
}

void orcac_config_free(orcac_config *cfg) {
  if (!cfg)
    return;
  free(cfg->executable);
  free(cfg->method);
  free(cfg->basis);
  free(cfg->keywords);
  free(cfg->scratch_dir);
  memset(cfg, 0, sizeof(*cfg));
}

int orcac_config_copy(orcac_config *dst, const orcac_config *src) {
  orcac_config tmp;
  memset(&tmp, 0, sizeof(tmp));
  tmp = *src;
  tmp.executable = tmp.method = tmp.basis = tmp.keywords = tmp.scratch_dir =
      NULL;
  if (set_cstr(&tmp.executable, src->executable) ||
      set_cstr(&tmp.method, src->method) || set_cstr(&tmp.basis, src->basis) ||
      set_cstr(&tmp.keywords, src->keywords) ||
      set_cstr(&tmp.scratch_dir, src->scratch_dir)) {
    orcac_config_free(&tmp);
    return -1;
  }
  *dst = tmp;
  return 0;
}

static int blank(const char *s) {
  if (!s)
    return 1;
  while (*s) {
    if (!isspace((unsigned char)*s))
      return 0;
    ++s;
  }
  return 1;
}

int orcac_config_validate(const orcac_config *cfg, char *err, size_t errlen) {
  if (blank(cfg->method)) {
    set_err(err, errlen, "orcac: method is required (ORCAParams.method)");
    return -1;
  }
  const struct {
    const char *name;
    const char *value;
  } single_line[] = {
      {"method", cfg->method},
      {"basis", cfg->basis},
      {"executable", cfg->executable},
      {"scratchDir", cfg->scratch_dir},
  };
  for (size_t i = 0; i < sizeof(single_line) / sizeof(single_line[0]); ++i) {
    if (single_line[i].value && strpbrk(single_line[i].value, "\r\n")) {
      set_err(err, errlen, "orcac: %s must not contain a line break",
              single_line[i].name);
      return -1;
    }
  }
  if (cfg->multiplicity < 1) {
    set_err(err, errlen, "orcac: multiplicity must be at least 1, got %d",
            cfg->multiplicity);
    return -1;
  }
  if (cfg->nprocs < 1) {
    set_err(err, errlen, "orcac: nprocs must be at least 1, got %d",
            cfg->nprocs);
    return -1;
  }
  if (cfg->maxcore_mb < 0) {
    set_err(err, errlen,
            "orcac: maxcoreMb must be zero (omit %%maxcore) or positive, got "
            "%d",
            cfg->maxcore_mb);
    return -1;
  }
  if (!isfinite(cfg->timeout_s) || cfg->timeout_s < 0.0) {
    set_err(err, errlen,
            "orcac: timeoutS must be a finite number of seconds, or 0 to wait "
            "without a limit");
    return -1;
  }
  return 0;
}

static int same_str(const char *a, const char *b) {
  return strcmp(a ? a : "", b ? b : "") == 0;
}

int orcac_config_same_wavefunction(const orcac_config *a,
                                   const orcac_config *b) {
  return same_str(a->method, b->method) && same_str(a->basis, b->basis) &&
         same_str(a->keywords, b->keywords) && a->charge == b->charge &&
         a->multiplicity == b->multiplicity &&
         same_str(a->executable, b->executable);
}

/* ---- Cap'n Proto ---- */

static int copy_text(char **slot, capn_text t) {
  return orcac_config_set_text(slot, t.str ? t.str : "",
                               t.len > 0 ? (size_t)t.len : 0);
}

static int open_root(struct capn *arena, const void *buf, size_t len,
                     capn_ptr *root, const char *what, char *err,
                     size_t errlen) {
  if (!buf || len == 0 || len % 8 != 0) {
    set_err(err, errlen,
            "orcac: %s must be an unpacked flat Cap'n Proto message (a "
            "non-empty multiple of 8 bytes)",
            what);
    return -1;
  }
  if (capn_init_mem(arena, (const uint8_t *)buf, len, 0) != 0) {
    set_err(err, errlen, "orcac: %s is not a Cap'n Proto message", what);
    return -1;
  }
  *root = capn_getp(capn_root(arena), 0, 1);
  if (root->type == CAPN_NULL) {
    capn_free(arena);
    set_err(err, errlen, "orcac: %s has no root struct", what);
    return -1;
  }
  return 0;
}

static int read_orca_params(ORCAParams_ptr ptr, orcac_config *out, char *err,
                            size_t errlen) {
  struct ORCAParams p;
  memset(&p, 0, sizeof(p));
  read_ORCAParams(&p, ptr);
  orcac_config cfg;
  if (orcac_config_init(&cfg) != 0) {
    set_err(err, errlen, "orcac: out of memory");
    return -1;
  }
  if (copy_text(&cfg.executable, p.executable) ||
      copy_text(&cfg.method, p.method) || copy_text(&cfg.basis, p.basis) ||
      copy_text(&cfg.keywords, p.keywords) ||
      copy_text(&cfg.scratch_dir, p.scratchDir)) {
    orcac_config_free(&cfg);
    set_err(err, errlen, "orcac: out of memory");
    return -1;
  }
  cfg.charge = p.charge;
  cfg.multiplicity = p.multiplicity;
  cfg.nprocs = p.nprocs;
  cfg.maxcore_mb = p.maxcoreMb;
  cfg.restart = p.restart ? 1 : 0;
  cfg.keep_files = p.keepFiles ? 1 : 0;
  cfg.timeout_s = p.timeoutS;
  *out = cfg;
  return 0;
}

int orcac_decode_params(const void *buf, size_t len, orcac_config *out,
                        char *err, size_t errlen) {
  struct capn arena;
  capn_ptr root;
  if (open_root(&arena, buf, len, &root, "ORCAParams", err, errlen) != 0)
    return -1;
  ORCAParams_ptr ptr;
  ptr.p = root;
  const int rc = read_orca_params(ptr, out, err, errlen);
  capn_free(&arena);
  return rc;
}

/* Relativity keywords ORCA takes on the simple-input line. */
static const char *relativity_keyword(const char *name) {
  static const char *const known[] = {"ZORA", "DKH", "DKH2", "X2C"};
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
    const char *k = known[i];
    size_t j = 0;
    while (k[j] && name[j] &&
           toupper((unsigned char)name[j]) == (unsigned char)k[j])
      ++j;
    if (!k[j] && !name[j])
      return k;
  }
  return NULL;
}

/* Lower CommonMethodSpec into cfg. Values the ORCA arm already sets win:
 * a non-empty method or basis, a non-zero charge, a multiplicity other
 * than 1. Fields ORCA has no lowering for are refused when set. */
static int lower_common(CommonMethodSpec_ptr ptr, orcac_config *cfg, char *err,
                        size_t errlen) {
  /* Generated readers load pointers lazily; resolve before a null check. */
  capn_resolve(&ptr.p);
  if (ptr.p.type == CAPN_NULL)
    return 0;
  struct CommonMethodSpec c;
  memset(&c, 0, sizeof(c));
  read_CommonMethodSpec(&c, ptr);

  if (c.planewaveCutoffEv != 0.0) {
    set_err(err, errlen,
            "orcac: common.planewaveCutoffEv has no ORCA lowering (ORCA uses "
            "Gaussian basis sets)");
    return -1;
  }
  /* Lists may sit behind far pointers; resolve before reading lengths. */
  capn_resolve(&c.xcFunctionals);
  if (capn_len(c.kMesh) > 0) {
    set_err(err, errlen,
            "orcac: common.kMesh has no ORCA lowering (ORCA is molecular)");
    return -1;
  }
  capn_resolve(&c.smearing.p);
  if (c.smearing.p.type != CAPN_NULL) {
    struct CommonMethodSpec_Smearing s;
    memset(&s, 0, sizeof(s));
    read_CommonMethodSpec_Smearing(&s, c.smearing);
    if (s.kind != CommonMethodSpec_Smearing_Kind_none || s.widthEv != 0.0) {
      set_err(err, errlen, "orcac: common.smearing has no ORCA lowering");
      return -1;
    }
  }
  if (c.scfEnergyToleranceEv != 0.0 || c.scfMaxIterations != 0) {
    set_err(err, errlen,
            "orcac: common SCF thresholds have no ORCA lowering; set them in "
            "ORCAParams.keywords (a %%scf block)");
    return -1;
  }
  if (c.vanDerWaalsMethod.len > 0 || c.vanDerWaalsS6 != 0.0) {
    set_err(err, errlen,
            "orcac: common dispersion settings have no ORCA lowering; put D3BJ "
            "or D4 in ORCAParams.keywords");
    return -1;
  }

  if (c.xcFunctionals.len > 0 && blank(cfg->method)) {
    if (c.xcFunctionals.len != 1) {
      set_err(err, errlen,
              "orcac: common.xcFunctionals lowers to ORCAParams.method only "
              "as a single composite name, got %d entries",
              c.xcFunctionals.len);
      return -1;
    }
    const capn_text none = {0, "", NULL};
    capn_text xc = capn_get_text(c.xcFunctionals, 0, none);
    if (copy_text(&cfg->method, xc)) {
      set_err(err, errlen, "orcac: out of memory");
      return -1;
    }
  }
  if (c.basisSet.len > 0 && blank(cfg->basis)) {
    if (c.basisSet.len == 9 && strncmp(c.basisSet.str, "planewave", 9) == 0) {
      set_err(err, errlen, "orcac: common.basisSet = planewave has no ORCA "
                           "lowering");
      return -1;
    }
    if (copy_text(&cfg->basis, c.basisSet)) {
      set_err(err, errlen, "orcac: out of memory");
      return -1;
    }
  }
  if ((c.hasCharge || c.charge != 0) && cfg->charge == 0)
    cfg->charge = c.charge;
  if (c.spinMultiplicity > 0 && cfg->multiplicity == 1)
    cfg->multiplicity = c.spinMultiplicity;
  if (c.relativityMethod.len > 0) {
    char name[32];
    const size_t n = (size_t)c.relativityMethod.len < sizeof(name) - 1
                         ? (size_t)c.relativityMethod.len
                         : sizeof(name) - 1;
    memcpy(name, c.relativityMethod.str, n);
    name[n] = '\0';
    const char *kw = relativity_keyword(name);
    if (!kw) {
      set_err(err, errlen,
              "orcac: common.relativityMethod '%s' has no ORCA lowering "
              "(ZORA, DKH, DKH2 or X2C)",
              name);
      return -1;
    }
    if (orcac_keywords_are_block(cfg->keywords)) {
      set_err(err, errlen,
              "orcac: common.relativityMethod cannot be merged into a verbatim "
              "keywords block; write it in the block");
      return -1;
    }
    const size_t old = strlen(cfg->keywords);
    char *joined = malloc(old + strlen(kw) + 2);
    if (!joined) {
      set_err(err, errlen, "orcac: out of memory");
      return -1;
    }
    snprintf(joined, old + strlen(kw) + 2, "%s%s%s", kw, old ? " " : "",
             cfg->keywords);
    free(cfg->keywords);
    cfg->keywords = joined;
  }
  return 0;
}

int orcac_decode_potential_config(const void *buf, size_t len,
                                  orcac_config *out, char *err,
                                  size_t errlen) {
  struct capn arena;
  capn_ptr root;
  if (open_root(&arena, buf, len, &root, "PotentialConfig", err, errlen) != 0)
    return -1;
  PotentialConfig_ptr ptr;
  ptr.p = root;
  struct PotentialConfig pc;
  memset(&pc, 0, sizeof(pc));
  read_PotentialConfig(&pc, ptr);
  capn_resolve(&pc.commonPatch.p);
  int rc = -1;
  if (pc.which != PotentialConfig_orca) {
    set_err(err, errlen,
            "orcac: PotentialConfig does not hold the orca arm (union "
            "discriminant %d)",
            (int)pc.which);
  } else if (pc.commonPatch.p.type != CAPN_NULL) {
    set_err(err, errlen,
            "orcac: PotentialConfig.commonPatch is not read by orcac; "
            "resolve it into common first");
  } else {
    orcac_config cfg;
    if (read_orca_params(pc.orca, &cfg, err, errlen) == 0) {
      if (lower_common(pc.common, &cfg, err, errlen) == 0) {
        *out = cfg;
        rc = 0;
      } else {
        orcac_config_free(&cfg);
      }
    }
  }
  capn_free(&arena);
  return rc;
}

int orcac_decode_potential_spec(const void *buf, size_t len, orcac_config *out,
                                char *err, size_t errlen) {
  struct capn arena;
  capn_ptr root;
  if (open_root(&arena, buf, len, &root, "PotentialSpec", err, errlen) != 0)
    return -1;
  PotentialSpec_ptr ptr;
  ptr.p = root;
  struct PotentialSpec spec;
  memset(&spec, 0, sizeof(spec));
  read_PotentialSpec(&spec, ptr);
  int rc = -1;
  if (spec.which != PotentialSpec_orca) {
    set_err(err, errlen,
            "orcac: PotentialSpec does not hold the orca arm (union "
            "discriminant %d)",
            (int)spec.which);
  } else {
    rc = read_orca_params(spec.orca, out, err, errlen);
  }
  capn_free(&arena);
  return rc;
}
