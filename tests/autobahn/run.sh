#!/bin/sh
# Runs the Autobahn|Testsuite fuzzing server (official container image) against the transport's
# client driver and summarizes the verdicts. Usage: tests/autobahn/run.sh <build-directory>
set -eu
build=$1
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'docker rm -f sp-autobahn >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
mkdir -p "$work/config" "$work/reports"
cp "$here/fuzzingserver.json" "$work/config/"
docker run -d --rm --name sp-autobahn --network host --user "$(id -u):$(id -g)" -v "$work/config:/config" -v "$work/reports:/reports" \
  crossbario/autobahn-testsuite wstest -m fuzzingserver -s /config/fuzzingserver.json >/dev/null
sleep 3
"$build/sp_ws_client_tool" autobahn ws://127.0.0.1:9001 schemaprovider
python3 "$here/summarize.py" "$work/reports/clients/index.json" schemaprovider
