#!/usr/bin/env bash
# tether demo driver: executes the narrated walkthrough of scripts/demo.md
# end-to-end and prints the transcript. Every refusal below is an EXPECTED
# output (that is the point of the confirm gate), so the script runs on -u
# only.
#
# Runtime model (console/main.cpp): one invocation = one verb. Each verb
# starts its own listener and pumps a bounded dial-in wait
# (agent_dial_wait_ms) for the agent's always-running dial-out loop; the
# dispatcher and background-job slots live in the agent process,
# independent of individual sessions.
#
# Usage: bash scripts/demo.sh [build-dir] [config-dir]
set -u

BIN="${1:-./build}"
CFG="${2:-./demo-run}"
TOKEN="demo-pre-shared-token-2026"

mkdir -p "$CFG"
cat > "$CFG/console.json" <<EOF
{"listen_address":"127.0.0.1","port":47474,"token_store_path":"$CFG/tokens.json","audit_path":"$CFG/audit.jsonl"}
EOF
printf '{"lab-1":"%s"}\n' "$TOKEN" > "$CFG/tokens.json"
cat > "$CFG/agent.json" <<EOF
{"console_address":"127.0.0.1","console_port":47474,"agent_name":"lab-1","token":"$TOKEN"}
EOF
echo "victim content" > "$CFG/victim.txt"
rm -f "$CFG/audit.jsonl" "$CFG/shot.png" "$CFG/frame.png" "$CFG/agent.out"

CONSOLE="$BIN/console/tether-console"
AGENT="$BIN/agent/tether-agent"
C() { "$CONSOLE" --config "$CFG/console.json" "$@"; }

echo "== 0. what we are running =="
ls -l "$CONSOLE" "$AGENT" | awk '{print $5, $NF}'

echo "== 1. agent (target) dials out with its config; the operator's console =="
echo "   is not a daemon - each upcoming verb invocation is a short visit"
nohup "$AGENT" --config "$CFG/agent.json" >"$CFG/agent.out" 2>&1 &
AGENT_PID=$!
echo "agent started (pid $AGENT_PID); dial-out loop running"

echo "== 2. registry listing =="
C agents

echo "== 3. process listing =="
C exec lab-1 list

echo "== 4. screenshot streamed to a file =="
C exec lab-1 screenshot > "$CFG/shot.png"
echo "exit=$? bytes=$(stat -c%s "$CFG/shot.png" 2>/dev/null || echo 0) magic=$(head -c4 "$CFG/shot.png" 2>/dev/null | od -An -tx1 | tr -d ' ')"

echo "== 5. background camera job lifecycle =="
C exec lab-1 start_webcam; echo "start exit=$?"
C exec lab-1 start_webcam; echo "duplicate-start exit=$?"
sleep 2
C exec lab-1 stop_webcam > "$CFG/frame.png"
echo "stop exit=$? bytes=$(stat -c%s "$CFG/frame.png" 2>/dev/null || echo 0) magic=$(head -c4 "$CFG/frame.png" 2>/dev/null | od -An -tx1 | tr -d ' ')"

echo "== 5b. background keylogger job (the same shared job registry) =="
C exec lab-1 start_keylogger "$CFG/keys.log"; echo "start exit=$?"
C exec lab-1 start_keylogger "$CFG/keys.log"; echo "duplicate-start exit=$?"
sleep 1
C exec lab-1 stop_keylogger > "$CFG/keys.out"
echo "stop exit=$? bytes=$(stat -c%s "$CFG/keys.out" 2>/dev/null || echo 0)"

echo "== 6. confirmation refusal, both gates =="
echo "victim before: $(head -1 "$CFG/victim.txt")"
C exec lab-1 delete "$CFG/victim.txt"; echo "gate1 (console, unconfirmed) exit=$?"
C exec lab-1 delete "$CFG/victim.txt" --yes; echo "gate2 (agent, no confirm token) exit=$?"
C exec lab-1 delete confirm "$CFG/victim.txt" --yes; echo "confirmed exit=$?"
[ -f "$CFG/victim.txt" ] && echo "victim still present after refusals" || echo "victim deleted"

echo "== 7. audit trail (console side) =="
wc -l "$CFG/audit.jsonl"

echo "== 8. teardown =="
kill "$AGENT_PID" 2>/dev/null
sleep 1
echo "demo complete"
