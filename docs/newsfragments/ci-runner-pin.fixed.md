CI runs on `ubuntu-24.04` instead of the floating `ubuntu-latest`, and
the sanitizer job installs the `libclang-rt-N-dev` that matches the
installed clang instead of a fixed `libclang-rt-18-dev`, so a runner
image with a newer clang no longer breaks the clang sanitizer leg.
