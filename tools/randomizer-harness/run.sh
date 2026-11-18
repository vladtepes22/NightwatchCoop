#!/bin/bash
# run.sh NAME WORLD(folder under work/harness/worlds, or none) SECONDS STEPSFILE PORT [extra --env NAME=VALUE ...]
# One headless solo game: takes the machine-wide lock (retry every 30 s), runs smoke-game.py, releases the lock.
H=$(cd "$(dirname "$0")" && pwd)
S=$(cd "$H/../.." && pwd)/work/harness; mkdir -p "$S/logs"
T=$(cd "$H/../.." && pwd)
LK="$(cygpath -w $H/lock.ps1)"
name=$1; world=$2; secs=$3; steps=$(realpath $4); port=$5; shift 5
[ -s "$steps" ] || { echo "no steps file $4"; exit 7; }
# turn rule (lead, 5 Nov): 90 s after our last release before taking the lock again; retry every 20 s
while :; do
  last=$(cat $S/last_release 2>/dev/null || echo 0)
  if [ $(( $(date +%s) - last )) -ge 90 ] && powershell -NoProfile -File "$LK" take "$name" >/dev/null 2>&1; then break; fi
  sleep 20
done
trap 'powershell -NoProfile -File "$LK" release >/dev/null 2>&1; date +%s > $S/last_release' EXIT
echo "$(date +%H:%M:%S) lock taken for $name"
export PATH="/c/msys64/clang64/bin:$PATH"
export BB_ARTIFACT_ROOT="$(cygpath -w $T/work/artifacts)"
mods=()
[ "$world" != none ] && mods=(--mods "$(cygpath -w $S/worlds/$world)")
cd $T
# quit the game 30 s after the chain's last step started (smoke-game otherwise waits for its full deadline)
LOG=$T/work/artifacts/smoke-$name/session.log
( until grep -qE "test: travel step ([0-9]+)/\1: |Party story: host #[0-9]+ ending 3" $LOG 2>/dev/null; do sleep 10; done; sleep 30
  echo '{"action": "quit"}' > $T/work/smoke-$name/user/party_command.json; echo "$(date +%H:%M:%S) chain done: quit sent" ) &
QUITTER=$!
/c/msys64/clang64/bin/python.exe -I tools/smoke-game.py --seconds "$secs" --name "$name" --new-game --port "$port" \
  --game 'C:\Games\CUSA03173' --engine "$(cygpath -w $T/engine)" "${mods[@]}" \
  --env "BB_PARTY_DIRECTOR_TEST=$(cat $steps)" --env BB_PARTY_DIRECTOR_TEST_SOLO=1 --env BB_PARTY_TARGET_ANIM=1 "$@" \
  > $S/logs/$name.txt 2>&1
code=$?
kill $QUITTER 2>/dev/null
echo "$(date +%H:%M:%S) $name exit $code"
tail -8 $S/logs/$name.txt
