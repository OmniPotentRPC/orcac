#pragma once

#include <stddef.h>

/* Everything the writer needs for one deck. Strings are borrowed. */
typedef struct OrcacDeck {
  const char *method;   /* NULL is "HF" */
  const char *basis;    /* NULL or "" omits it */
  const char *keywords; /* single line; NULL or "" omits it */
  const char *blocks;   /* validated % blocks; NULL or "" omits them */
  int charge;
  int multiplicity;     /* <= 0 is 1 */
  int nprocs;           /* > 1 writes %pal */
  int maxcore_mb;       /* > 0 writes %maxcore */
} OrcacDeck;

/*
 * Render an EnGrad deck. The orcac-owned blocks (%maxcore, %pal,
 * %scf ConvForced and %moinp) follow the user blocks, so they win if
 * ORCA merges repeated blocks.
 */
int orcac_render_input(const OrcacDeck *deck, int n_atoms,
                       const double *positions_ang,
                       const int *atomic_numbers,
                       const char *moinp_name,
                       char **input_text,
                       char *errbuf,
                       size_t errbuf_n);

/*
 * field is a single simple-input line: no control characters other than
 * tab. what names the field in the error text.
 */
int orcac_validate_line(const char *what, const char *text, char *errbuf,
                        size_t errbuf_n);

/*
 * method or basis is one simple-input token: letters, digits and
 * -+*(),/_.' only, so it cannot smuggle in a second keyword, a comment
 * or a block, and it is not PAL<n>, MORead or ConvForced. NULL and ""
 * pass.
 */
int orcac_validate_token(const char *what, const char *text, char *errbuf,
                         size_t errbuf_n);

/*
 * keywords is a single line and names nothing orcac owns: no '%', no
 * '#', no PAL<n>, no MORead, no ConvForced.
 */
int orcac_validate_keywords(const char *keywords, char *errbuf,
                            size_t errbuf_n);

/*
 * blocks holds only user % blocks: no carriage returns or control
 * characters other than newline and tab, no '!' or '*' lines, no
 * $new_job, no %pal, %maxcore, %moinp, %output, %base or %coords, and
 * no ConvForced or MORead anywhere.
 */
int orcac_validate_blocks(const char *blocks, char *errbuf, size_t errbuf_n);

const char *orcac_element_symbol(int atomic_number);
