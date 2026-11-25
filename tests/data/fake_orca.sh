#!/bin/sh
# Hand-written stand-in for the ORCA binary. Not an ORCA build and not the
# output of an ORCA run. The orcac tests name this script as the executable.
#
# Environment knobs (all optional):
#   ORCAC_FAKE_ENGRAD       .engrad fixture copied to <base>.engrad
#   ORCAC_FAKE_LOG          append the received input and cwd here
#   ORCAC_FAKE_ECHO_INPUT   echo the input to stdout the way ORCA frames it
#   ORCAC_FAKE_STDOUT       file copied to stdout before exit
#   ORCAC_FAKE_SCF_FAIL     print an unconverged-SCF report, exit 1
#   ORCAC_FAKE_SCF_FAIL_EXIT0  same report, exit 0 with an .engrad
#   ORCAC_FAKE_MOREAD_FAIL  fail like an SCF failure when the input has MORead
#   ORCAC_FAKE_FAIL         exit with this status after an error line
#   ORCAC_FAKE_NO_ENGRAD    exit 0 without an .engrad
#   ORCAC_FAKE_BAD_ENGRAD   write an .engrad that is not one
#   ORCAC_FAKE_SLEEP        sleep this many seconds first
#   ORCAC_FAKE_PIDFILE      write the shell pid here (timeout test)
set -u

inp=
for a in "$@"; do
  case "$a" in
    *.inp) inp=$a ;;
  esac
done
if [ -z "$inp" ]; then
  echo "fake orca: no .inp argument" >&2
  exit 2
fi
base=${inp%.inp}

if [ -n "${ORCAC_FAKE_LOG:-}" ]; then
  printf '=== %s ===\n' "$inp" >> "$ORCAC_FAKE_LOG"
  printf 'CWD %s\n' "$(pwd)" >> "$ORCAC_FAKE_LOG"
  if grep -q '^%moinp' "$inp"; then
    moinp=$(sed -n 's/^%moinp "\(.*\)"$/\1/p' "$inp")
    if [ -s "$moinp" ]; then
      printf 'MOINP present %s\n' "$moinp" >> "$ORCAC_FAKE_LOG"
    else
      printf 'MOINP missing %s\n' "$moinp" >> "$ORCAC_FAKE_LOG"
    fi
  fi
  cat "$inp" >> "$ORCAC_FAKE_LOG"
fi

if [ -n "${ORCAC_FAKE_PIDFILE:-}" ]; then
  echo $$ > "$ORCAC_FAKE_PIDFILE"
fi
if [ -n "${ORCAC_FAKE_SLEEP:-}" ]; then
  sleep "$ORCAC_FAKE_SLEEP"
fi
echo "fake orca stderr line" >&2

if [ "${ORCAC_FAKE_ECHO_INPUT:-}" = 1 ]; then
  echo "================================================================================"
  echo "                                       INPUT FILE"
  echo "================================================================================"
  echo "NAME = $inp"
  awk '{ printf "|%3d> %s\n", NR, $0 }
       END { printf "|%3d>                          ****END OF INPUT****\n", NR + 1 }' "$inp"
  echo "================================================================================"
fi
if [ -n "${ORCAC_FAKE_STDOUT:-}" ]; then
  cat "$ORCAC_FAKE_STDOUT"
fi
if [ "${ORCAC_FAKE_MOREAD_FAIL:-}" = 1 ] && grep -q 'MORead' "$inp"; then
  echo "SCF NOT CONVERGED AFTER 1 CYCLES"
  echo "ORCA finished by error termination in SCF"
  exit 1
fi
if [ -n "${ORCAC_FAKE_SCF_FAIL:-}" ]; then
  echo "SCF NOT CONVERGED AFTER 5 CYCLES"
  echo "ORCA finished by error termination in SCF"
  exit 1
fi
if [ -n "${ORCAC_FAKE_FAIL:-}" ]; then
  echo "ORCA finished by error termination"
  echo "fake orca forced failure"
  exit "$ORCAC_FAKE_FAIL"
fi
if [ -n "${ORCAC_FAKE_BAD_ENGRAD:-}" ]; then
  echo "this is not an engrad file" > "${base}.engrad"
  exit 0
fi
if [ -n "${ORCAC_FAKE_NO_ENGRAD:-}" ]; then
  echo "ORCA TERMINATED NORMALLY"
  exit 0
fi
src=${ORCAC_FAKE_ENGRAD:-}
if [ -z "$src" ]; then
  echo "fake orca: ORCAC_FAKE_ENGRAD is unset" >&2
  exit 2
fi
cp "$src" "${base}.engrad"
printf 'gbw' > "${base}.gbw"
if [ -n "${ORCAC_FAKE_SCF_FAIL_EXIT0:-}" ]; then
  echo "SCF NOT CONVERGED AFTER 125 CYCLES"
  exit 0
fi
echo "ORCA TERMINATED NORMALLY"
exit 0
