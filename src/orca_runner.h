#pragma once

#include <stddef.h>

typedef struct OrcacChildResult {
  int exited;
  int exit_code;
  int signal;
  int timed_out;
} OrcacChildResult;

int orcac_resolve_executable(const char *requested,
                             char *resolved,
                             size_t resolved_n,
                             char *errbuf,
                             size_t errbuf_n);

int orcac_make_run_dir(const char *scratch_root,
                       char *run_dir,
                       size_t run_dir_n,
                       char *errbuf,
                       size_t errbuf_n);

int orcac_run_orca(const char *executable,
                   const char *run_dir,
                   const char *input_name,
                   const char *stdout_path,
                   const char *stderr_path,
                   int timeout_s,
                   OrcacChildResult *result,
                   char *errbuf,
                   size_t errbuf_n);

int orcac_remove_tree(const char *path);
