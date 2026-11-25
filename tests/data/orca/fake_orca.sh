#!/bin/sh
# Hand-written stand-in for the ORCA binary. Not an ORCA build and not
# the output of an ORCA run. The tests point executable here.
# ORCAC_FAKE_REPLAY is the directory holding the synthetic fixtures.
#
# Modes, all optional:
#   ORCAC_FAKE_LOG           append the cwd, its files and the input deck
#                            to this file
#   ORCAC_FAKE_STARTED       create this file once the run has started
#   ORCAC_FAKE_ENV_LOG       write the child environment to this file
#   ORCAC_FAKE_SYMLINK_TARGET  plant symlinks to this directory and to
#                            its file "precious" in the run directory
#   ORCAC_FAKE_LOCKED        leave a directory "locked" holding a file,
#                            with its write permission removed
#   ORCAC_FAKE_SLEEP         sleep this many seconds first
#   ORCAC_FAKE_STRAY_PIDFILE leave a background child, write its pid here
#   ORCAC_FAKE_FAIL          print an error and exit with this status
#   ORCAC_FAKE_NOCONV        print the unconverged-SCF fixture and exit
#                            with this status; 0 also writes the outputs
#   ORCAC_FAKE_FAIL_MOREAD   like ORCAC_FAKE_NOCONV=1, only for decks
#                            that request MORead
#   ORCAC_FAKE_FD_LOG        list this shell's open descriptors here
#   ORCAC_FAKE_ENGRAD        fixture copied as the .engrad instead of
#                            replay.engrad
#   ORCAC_FAKE_STDOUT        fixture printed instead of replay.stdout
#   ORCAC_FAKE_ECHO_INPUT    print an input echo block, "|  n> line" for
#                            each deck line, before the fixture output
#   ORCAC_FAKE_GBW_LINK      write the .gbw as a symlink to this path
#   ORCAC_FAKE_ENGRAD_LINK   write the .engrad as a symlink to this path
set -u

log=${ORCAC_FAKE_LOG:-}
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
if [ -n "$log" ]; then
  printf '=== %s ===\n' "$inp" >> "$log"
  printf 'CWD %s\n' "$(pwd)" >> "$log"
  printf 'FILES %s\n' "$(/bin/ls -A | /usr/bin/tr '\n' ' ')" >> "$log"
  /bin/cat "$inp" >> "$log"
fi
if [ -n "${ORCAC_FAKE_STARTED:-}" ]; then
  : > "${ORCAC_FAKE_STARTED}"
fi
if [ -n "${ORCAC_FAKE_ENV_LOG:-}" ]; then
  /usr/bin/env > "${ORCAC_FAKE_ENV_LOG}"
fi
if [ -n "${ORCAC_FAKE_FD_LOG:-}" ]; then
  /bin/ls -l "/proc/$$/fd" > "${ORCAC_FAKE_FD_LOG}" 2>&1
fi
if [ -n "${ORCAC_FAKE_SYMLINK_TARGET:-}" ]; then
  /bin/ln -s "${ORCAC_FAKE_SYMLINK_TARGET}" escape_dir
  /bin/ln -s "${ORCAC_FAKE_SYMLINK_TARGET}/precious" escape_file
  /bin/mkdir nested
  /bin/ln -s "${ORCAC_FAKE_SYMLINK_TARGET}" nested/escape_dir
fi
if [ -n "${ORCAC_FAKE_LOCKED:-}" ]; then
  /bin/mkdir locked
  : > locked/file
  /bin/chmod 0500 locked
fi

if [ -n "${ORCAC_FAKE_SLEEP:-}" ]; then
  /bin/sleep "${ORCAC_FAKE_SLEEP}"
fi
if [ -n "${ORCAC_FAKE_STRAY_PIDFILE:-}" ]; then
  /bin/sleep 30 &
  echo $! > "${ORCAC_FAKE_STRAY_PIDFILE}"
fi
if [ -n "${ORCAC_FAKE_FAIL:-}" ]; then
  echo "ORCA finished by error termination"
  echo "fake orca forced failure"
  echo "fake orca stderr line" >&2
  exit "${ORCAC_FAKE_FAIL}"
fi
replay=${ORCAC_FAKE_REPLAY:-}
if [ -z "$replay" ]; then
  echo "fake orca: ORCAC_FAKE_REPLAY is unset" >&2
  exit 2
fi
if [ -n "${ORCAC_FAKE_ECHO_INPUT:-}" ]; then
  printf '%s\n' '==========' '                INPUT FILE' '==========' \
    "NAME = $inp"
  n=0
  while IFS= read -r l || [ -n "$l" ]; do
    n=$((n + 1))
    printf '|%3d> %s\n' "$n" "$l"
  done < "$inp"
  printf '|%3d>                  ****END OF INPUT****\n' "$((n + 1))"
  printf '%s\n' '=========='
fi
noconv=${ORCAC_FAKE_NOCONV:-}
if [ -n "${ORCAC_FAKE_FAIL_MOREAD:-}" ] && /bin/grep -q 'MORead' "$inp"; then
  noconv=1
fi
if [ -n "$noconv" ]; then
  /bin/cat "$replay/scf_not_converged.stdout"
  if [ "$noconv" = 0 ]; then
    /bin/cp "$replay/replay.engrad" "${base}.engrad"
    printf 'gbw' > "${base}.gbw"
  fi
  exit "$noconv"
fi
if [ -n "${ORCAC_FAKE_ENGRAD_LINK:-}" ]; then
  /bin/ln -s "${ORCAC_FAKE_ENGRAD_LINK}" "${base}.engrad"
else
  /bin/cp "$replay/${ORCAC_FAKE_ENGRAD:-replay.engrad}" "${base}.engrad"
fi
/bin/cat "$replay/${ORCAC_FAKE_STDOUT:-replay.stdout}"
if [ -n "${ORCAC_FAKE_GBW_LINK:-}" ]; then
  /bin/ln -s "${ORCAC_FAKE_GBW_LINK}" "${base}.gbw"
else
  printf 'gbw' > "${base}.gbw"
fi
exit 0
