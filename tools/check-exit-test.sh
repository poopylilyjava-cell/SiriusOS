#!/usr/bin/env bash
#
# Check a serial log from the default boot entry against the M2 exit test:
# two user programs ran on different cores, their output interleaved, and
# both exited cleanly.
#
#   ./tools/check-exit-test.sh boot.log

set -euo pipefail

LOG="${1:?usage: check-exit-test.sh <serial log>}"
fail() { printf 'exit test FAILED: %s\n' "$*" >&2; exit 1; }

grep -q "sirius: all user processes exited" "$LOG" || fail "the user programs did not all exit"
grep -q "\[alpha\] done" "$LOG" || fail "alpha did not finish"
grep -q "\[beta\] done"  "$LOG" || fail "beta did not finish"
! grep -q "exited with status [1-9]" "$LOG" || fail "a program exited with a nonzero status"

# Each ticker line ends in "cpu N". A thread never migrates in M2, so each
# program reports a single CPU, and the two must differ.
cpus_of() { grep -oE "^\[$1\] tick .* cpu [0-9]+" "$LOG" | awk '{print $NF}' | sort -u; }
alpha_cpus="$(cpus_of alpha)"
beta_cpus="$(cpus_of beta)"
[ -n "$alpha_cpus" ] && [ -n "$beta_cpus" ] || fail "no tick lines found"
[ "$(comm -12 <(echo "$alpha_cpus") <(echo "$beta_cpus") | wc -l)" -eq 0 ] ||
    fail "alpha and beta shared a cpu ($alpha_cpus / $beta_cpus)"

# Interleaved: in the order the lines arrived, the speaker changes more than
# once. Strictly one program's output followed by the other's changes once.
switches="$(grep -oE '^\[(alpha|beta)\] tick' "$LOG" | awk '$1 != prev { n++ } { prev = $1 } END { print n - 1 }')"
[ "$switches" -ge 2 ] || fail "output was not interleaved ($switches switches)"

printf 'exit test ok: alpha on cpu %s, beta on cpu %s, %s switches between them\n' \
    "$alpha_cpus" "$beta_cpus" "$switches"
