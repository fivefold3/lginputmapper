#!/bin/sh
# Installs the freshly packaged ipk on the TV over SSH (host alias from ~/.ssh/config).
# Usage: scripts/deploy.sh [ssh-host]   (default: lgtv). Installs, restarts the daemon and launches the app.
set -eu
HOST="${1:-lgtv}"
cd "$(dirname "$0")/.."
IPK=$(ls -t dist/*.ipk | head -1)
[ -n "$IPK" ] || { echo "no ipk in dist/, run npm run package first"; exit 1; }
echo "uploading $IPK to $HOST:/tmp/"
ssh "$HOST" 'cat > /tmp/lginputmapper.ipk' < "$IPK"
echo "installing"
SUM=$(md5 -q dist/service/service.js 2>/dev/null || md5sum dist/service/service.js | cut -d" " -f1)
# The install subscription never ends on its own: fire it, then wait until the installed service.js matches our build.
ssh "$HOST" 'luna-send -n 1 luna://com.webos.appInstallService/dev/install '"'"'{"id":"com.ares.defaultName","ipkUrl":"/tmp/lginputmapper.ipk","subscribe":true}'"'"' >/dev/null 2>&1 & sleep 2; for i in $(seq 1 60); do S=$(md5sum /media/developer/apps/usr/palm/services/com.lginputmapper.app.service/service.js 2>/dev/null | cut -d" " -f1); if [ "$S" = "'"$SUM"'" ]; then echo "installed"; rm -f /tmp/lginputmapper.ipk; exit 0; fi; sleep 1; done; echo "install did not complete"; exit 1'
echo "restarting service and daemon"
# Stop the old daemon first (an old build could hold the service's hub socket), then the old service
# instance. A shell luna-send cannot wake a dev service (the bus drops it), so the app is launched
# instead: its setup call starts the service, which starts the new daemon.
ssh "$HOST" 'P=$(cat /tmp/lginputmapperd/pid 2>/dev/null); [ -n "$P" ] && kill $P 2>/dev/null; for p in $(pgrep -f "[c]om.lginputmapper.app.service"); do kill $p; done; sleep 2; rm -f /tmp/lginputmapperd/pid; luna-send -n 1 luna://com.webos.applicationManager/launch '"'"'{"id":"com.lginputmapper.app"}'"'"' </dev/null >/dev/null 2>&1; for i in $(seq 1 20); do [ -f /tmp/lginputmapperd/pid ] && { echo "app launched, daemon pid $(cat /tmp/lginputmapperd/pid)"; exit 0; }; sleep 1; done; echo "daemon did not start"; exit 1'
