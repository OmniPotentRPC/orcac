#pragma once

#include <orcac.h>

#include <stddef.h>

int orcac_render_input(const OrcaCConfig *cfg, int n_atoms,
                       const double *positions_ang,
                       const int *atomic_numbers,
                       const char *moinp_name,
                       char **input_text,
                       char *errbuf,
                       size_t errbuf_n);

const char *orcac_element_symbol(int atomic_number);
