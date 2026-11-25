/* Cap'n Proto Capabilities discovery for the orcac backend. */
#include "orcac.h"

#include "potential.capnp.h"

#include <stdint.h>
#include <string.h>

#ifndef ORCAC_SCHEMA_VERSION
#define ORCAC_SCHEMA_VERSION ""
#endif

static capn_text text_of(const char *s) {
  capn_text t = {(int)strlen(s), s, NULL};
  return t;
}

static int set_text_list(capn_ptr list, const char *const *items,
                         size_t count) {
  for (size_t i = 0; i < count; ++i)
    if (capn_set_text(list, (int)i, text_of(items[i])) != 0)
      return -1;
  return 0;
}

int orcac_capabilities_result(void *capabilities_capnp,
                              size_t capabilities_capnp_capacity_bytes,
                              size_t *capabilities_capnp_size_bytes) {
  /* CommonMethodSpec fields the PotentialConfig overlay lowers into the ORCA
   * arm (orcac_params.c). Every other set field is refused. */
  static const char *const lowered_common_fields[] = {
      "xcFunctionals", "basisSet",         "charge",
      "hasCharge",     "spinMultiplicity", "relativityMethod",
  };
  /* ORCA runs EnGrad: energy, gradient and forces come from one .engrad. */
  static const uint16_t operations[] = {
      Capabilities_Operation_energy,
      Capabilities_Operation_forces,
      Capabilities_Operation_gradient,
  };
  static const char *const config_kinds[] = {"orca"};

  if (!capabilities_capnp_size_bytes)
    return -1;
  *capabilities_capnp_size_bytes = 0;

  struct capn arena;
  capn_init_malloc(&arena);
  capn_ptr root = capn_root(&arena);
  if (root.type == CAPN_NULL) {
    capn_free(&arena);
    return -1;
  }

  const size_t n_ops = sizeof(operations) / sizeof(operations[0]);
  const size_t n_lowered =
      sizeof(lowered_common_fields) / sizeof(lowered_common_fields[0]);
  const size_t n_kinds = sizeof(config_kinds) / sizeof(config_kinds[0]);

  Capabilities_ptr caps = new_Capabilities(root.seg);
  capn_list16 op_list = capn_new_list16(root.seg, (int)n_ops);
  capn_ptr lowered_list = capn_new_ptr_list(root.seg, (int)n_lowered);
  capn_ptr kind_list = capn_new_ptr_list(root.seg, (int)n_kinds);
  if (caps.p.type == CAPN_NULL || op_list.p.type == CAPN_NULL ||
      lowered_list.type == CAPN_NULL || kind_list.type == CAPN_NULL) {
    capn_free(&arena);
    return -1;
  }
  for (size_t i = 0; i < n_ops; ++i)
    capn_set16(op_list, (int)i, operations[i]);
  if (set_text_list(lowered_list, lowered_common_fields, n_lowered) != 0 ||
      set_text_list(kind_list, config_kinds, n_kinds) != 0) {
    capn_free(&arena);
    return -1;
  }

  const char *version = orcac_version();
  struct Capabilities view;
  memset(&view, 0, sizeof(view));
  view.backendName = text_of("orcac");
  view.backendVersion = text_of(version ? version : "");
  view.abiVersion = orcac_abi_version();
  view.available = orcac_available() ? 1 : 0;
  view.operations = op_list;
  view.loweredCommonFields = lowered_list;
  view.configKinds = kind_list;
  view.schemaVersion = text_of(ORCAC_SCHEMA_VERSION);
  view.protocolFamily = text_of("rgpot.potentials");
  view.protocolMajor = 1;
  view.protocolMinor = 2; /* const protocolMinor in schema/potential.capnp */
  view.schemaId = text_of("bd1f89fa17369103");
  view.bridgeAbiMajor = 1;
  view.bridgeAbiMinor = 0;
  view.bridgeLayout = 1;
  view.dlpackMajor = 1;
  view.dlpackMinor = 0;
  view.bridgeFeatures = 0;
  view.buildVersion = text_of(version ? version : "");
  view.buildRevision = text_of("");
  write_Capabilities(&view, caps);
  if (capn_setp(root, 0, caps.p) != 0) {
    capn_free(&arena);
    return -1;
  }

  uint8_t scratch[2048];
  const int written = capn_write_mem(&arena, scratch, sizeof(scratch), 0);
  capn_free(&arena);
  if (written < 0)
    return -1;
  *capabilities_capnp_size_bytes = (size_t)written;
  if (!capabilities_capnp ||
      capabilities_capnp_capacity_bytes < (size_t)written)
    return -1;
  memcpy(capabilities_capnp, scratch, (size_t)written);
  return 0;
}
