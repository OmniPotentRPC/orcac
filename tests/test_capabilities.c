/* orcac_capabilities_result round-trips through the generated reader. */
#include "orcac_test_util.h"

static int text_is(capn_text t, const char *want) {
  return t.len == (int)strlen(want) && memcmp(t.str, want, (size_t)t.len) == 0;
}

static const capn_text k_none = {0, "", NULL};

int main(void) {
  size_t size = 0;
  CHECK(orcac_capabilities_result(NULL, 0, &size) == -1);
  CHECK(size > 0 && size % 8 == 0);
  CHECK(orcac_capabilities_result(NULL, 0, NULL) == -1);
  uint8_t *buf = malloc(size);
  size_t written = 0;
  CHECK(orcac_capabilities_result(buf, size, &written) == 0);
  CHECK(written == size);

  struct capn arena;
  CHECK(capn_init_mem(&arena, buf, written, 0) == 0);
  Capabilities_ptr ptr;
  ptr.p = capn_getp(capn_root(&arena), 0, 1);
  struct Capabilities caps;
  read_Capabilities(&caps, ptr);
  CHECK(text_is(caps.backendName, "orcac"));
  CHECK(text_is(caps.backendVersion, orcac_version()));
  CHECK(text_is(caps.buildVersion, orcac_version()));
  CHECK(caps.abiVersion == ORCAC_ABI_VERSION);
  CHECK((int)caps.available == orcac_available());
  CHECK(text_is(caps.protocolFamily, "rgpot.potentials"));
  CHECK(caps.protocolMajor == 1);
  CHECK(caps.protocolMinor == 2);
  CHECK(text_is(caps.schemaId, "bd1f89fa17369103"));
  CHECK(caps.schemaVersion.len > 7 &&
        strncmp(caps.schemaVersion.str, "rgconf@", 7) == 0);

  CHECK(capn_len(caps.operations) == 3);
  CHECK(capn_get16(caps.operations, 0) == Capabilities_Operation_energy);
  CHECK(capn_get16(caps.operations, 1) == Capabilities_Operation_forces);
  CHECK(capn_get16(caps.operations, 2) == Capabilities_Operation_gradient);

  capn_resolve(&caps.configKinds);
  capn_resolve(&caps.loweredCommonFields);
  CHECK(caps.configKinds.len == 1);
  CHECK(text_is(capn_get_text(caps.configKinds, 0, k_none), "orca"));
  int saw_basis = 0;
  for (int i = 0; i < caps.loweredCommonFields.len; ++i)
    if (text_is(capn_get_text(caps.loweredCommonFields, i, k_none),
                "basisSet"))
      saw_basis = 1;
  CHECK(saw_basis);

  capn_free(&arena);
  free(buf);
  return test_finish_main("capabilities");
}
