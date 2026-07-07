#!/usr/bin/env bash
# repro_live_signup.sh — RELIABLE reproduction of the depot wire-protocol
# parameter-corruption bug, via forgepm's LIVE server.
#
# The bug only manifests in the running HTTP server (NOT in a standalone loop —
# see test_wire_param_repro.march, which runs clean 0/100). forgepm's POST /signup
# calls create_user, which does the create_user-shaped INSERT (4 ParamText, one a
# long pbkdf2 hash). Repeated signups intermittently fail with a create_user
# db_error (corrupt bytes rejected by Postgres) and/or CRASH the server (SIGSEGV).
#
# Usage:   bash /Users/80197052/code/depot/test/repro_live_signup.sh [N]   (default 24)
# Needs:   Postgres up with forgepm_dev seeded; forgepm at /Users/80197052/code/forgepm.
# CLEAN build -> "OK=N CORRUPTION=0 CRASH=0".  BUG present -> CORRUPTION>0 and/or CRASH>0.
#
# SERVER=interp tests the interpreter (forge run); both reproduce (interpreter uses
# OCaml's GC, so this is NOT a Perceus reference-counting bug).
#
# NOTE on rate limiting: forgepm caps signups at 10/min/IP (HTTP 429). To keep the
# limiter out of the measurement, each BATCH is a FRESH server (resets the in-memory
# limiter) doing up to 9 signups, stopping the batch at the first 429.
set -u
N="${1:-24}"
SERVER="${SERVER:-compiled}"     # compiled | interp
FP=/Users/80197052/code/forgepm
B=http://127.0.0.1:4000

cd "$FP" || exit 1
[ "$SERVER" = "compiled" ] && { echo "# building forgepm..."; forge build >/tmp/repro_build.log 2>&1; }

start_server() {
  lsof -ti :4000 2>/dev/null | xargs kill -9 2>/dev/null
  pkill -9 -f "build/debug/forgepm" 2>/dev/null; pkill -9 -f "forge run" 2>/dev/null; sleep 2
  if [ "$SERVER" = "interp" ]; then
    MARCH_ENV=dev forge run >/tmp/repro_srv.log 2>&1 &
    local n=0; until [ -n "$(lsof -ti :4000 2>/dev/null)" ] || [ $n -ge 25 ]; do sleep 2; n=$((n+1)); done
  else
    MARCH_ENV=dev ./.march/build/debug/forgepm >/tmp/repro_srv.log 2>&1 &
    sleep 4
  fi
}

BATCHES=$(( (N + 8) / 9 ))
ok=0; corrupt=0; crash=0
for b in $(seq 1 "$BATCHES"); do
  start_server
  [ -z "$(lsof -ti :4000 2>/dev/null)" ] && { echo "server did not start; see /tmp/repro_srv.log"; exit 1; }
  for s in $(seq 1 9); do
    if [ -z "$(lsof -ti :4000 2>/dev/null)" ]; then crash=$((crash+1)); echo "  *** SERVER CRASHED (batch $b) ***"; break; fi
    J=$(mktemp); U="rp$(openssl rand -hex 4)"
    TOK=$(curl -s --max-time 10 -c "$J" "$B/signup" | grep -o 'name="_csrf_token" value="[^"]*"' | sed 's/.*value="//;s/"//')
    ST=$(curl -s --max-time 12 -b "$J" -c "$J" -o /dev/null -w '%{http_code}' -X POST \
         --data-urlencode "username=$U" --data-urlencode "email=$U@t.test" \
         --data-urlencode "password=hunter2pass" --data-urlencode "_csrf_token=$TOK" "$B/signup" 2>/dev/null)
    rm -f "$J"
    case "$ST" in
      302) ok=$((ok+1)) ;;
      429) break ;;                       # rate limit; rest of batch would 429 too
      000) crash=$((crash+1)); echo "  *** SERVER CRASHED (batch $b, signup $s) ***"; break ;;
      *)   corrupt=$((corrupt+1)); echo "  CORRUPTION (batch $b, signup $s): http=$ST (create_user db_error)" ;;
    esac
  done
done

echo "=== SERVER=$SERVER  OK=$ok  CORRUPTION=$corrupt  CRASH=$crash  over $BATCHES batches ==="
echo "    BUG PRESENT iff CORRUPTION>0 or CRASH>0.  A fixed build -> CORRUPTION=0, CRASH=0."
lsof -ti :4000 2>/dev/null | xargs kill -9 2>/dev/null
pkill -9 -f "build/debug/forgepm" 2>/dev/null; pkill -9 -f "forge run" 2>/dev/null
