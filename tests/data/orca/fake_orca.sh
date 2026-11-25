#!/bin/sh
# Hand-written stand-in for the ORCA binary. Not an ORCA build and not
# the output of an ORCA run. test_orcac_replay points orca_bin here.
# ORCAC_FAKE_REPLAY is the directory holding the synthetic fixtures.
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
  /bin/cat "$inp" >> "$log"
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
/bin/cp "$replay/replay.engrad" "${base}.engrad"
/bin/cat "$replay/replay.stdout"
printf 'gbw' > "${base}.gbw"
exit 0
