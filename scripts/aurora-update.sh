#!/usr/bin/env bash
# Aurora self-update, run by aurora-update.timer (installed by
# scripts/install-updater.sh). It fetches the firmware branch from GitHub and,
# when the branch has moved forward, builds it, runs the unit tests, installs
# it and restarts wallet-fw. A failed build or test keeps the installed
# version; a service that will not stay up afterwards gets the previous binary
# back. A commit that failed is not retried. Log: journalctl -u aurora-update
#
#   aurora-update            check and update
#   aurora-update --check    only check that GitHub can be reached
set -euo pipefail

: "${CONF:=/etc/aurora-update}"
: "${STATE:=/var/lib/aurora-update}"
: "${BIN:=/usr/local/bin/wallet-fw}"
: "${SYSTEMCTL:=systemctl}"
: "${HEALTH_WAIT:=20}"
: "${SERVICE:=/etc/systemd/system/wallet-fw.service}"
REPO_URL=https://github.com/blockchain-wallet-hackathon/wallet-firmware.git
BRANCH=firmware
# shellcheck source=/dev/null
[ -f "$CONF/update.conf" ] && . "$CONF/update.conf"
SRC="$STATE/src"

log() { echo "aurora-update: $*"; }

# git with the read-only credentials from install-updater.sh: a deploy key
# (SSH URL) or a token (HTTPS). Passed through the environment, not argv.
git_() {
  local env=(GIT_TERMINAL_PROMPT=0 GIT_CONFIG_COUNT=2
             GIT_CONFIG_KEY_0=credential.helper GIT_CONFIG_VALUE_0=
             GIT_CONFIG_KEY_1=safe.directory GIT_CONFIG_VALUE_1="$SRC")
  if [ -f "$CONF/token" ]; then
    local auth
    auth=$(printf 'x-access-token:%s' "$(cat "$CONF/token")" | base64 -w0)
    env+=(GIT_CONFIG_COUNT=3 GIT_CONFIG_KEY_2=http.extraHeader
          GIT_CONFIG_VALUE_2="Authorization: Basic $auth")
  fi
  if [ -f "$CONF/deploy_key" ]; then
    env+=(GIT_SSH_COMMAND="ssh -i $CONF/deploy_key -o IdentitiesOnly=yes -o BatchMode=yes \
-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$CONF/known_hosts")
  fi
  env "${env[@]}" git "$@"
}

if [ "${1:-}" = --check ]; then
  git_ ls-remote --exit-code "$REPO_URL" "refs/heads/$BRANCH" >/dev/null
  log "GitHub reachable: $REPO_URL ($BRANCH)"
  exit 0
fi

mkdir -p "$STATE"
if [ ! -d "$SRC/.git" ]; then
  rm -rf "$SRC"
  git_ clone --quiet --branch "$BRANCH" --single-branch "$REPO_URL" "$SRC"
fi
cd "$SRC"
git_ remote set-url origin "$REPO_URL"
git_ fetch --quiet origin "$BRANCH"
NEW=$(git_ rev-parse FETCH_HEAD)
SHORT=${NEW:0:7}
CURRENT=$(cat "$STATE/installed" 2>/dev/null || true)
if [ "$NEW" = "$CURRENT" ]; then
  log "up to date ($SHORT)"
  exit 0
fi
if grep -qxF "$NEW" "$STATE/failed" 2>/dev/null; then
  log "$SHORT failed before; waiting for a newer commit"
  exit 0
fi

fail() {
  log "$SHORT: $1; keeping the installed version"
  echo "$NEW" >> "$STATE/failed"
  exit 1
}
# Only move forward along the branch: rewritten history is never installed.
if [ -n "$CURRENT" ] && ! git_ merge-base --is-ancestor "$CURRENT" "$NEW"; then
  fail "the branch no longer contains the installed commit (history rewritten)"
fi
# Optional: accept only commits signed by a key in allowed_signers (SSH format).
if [ -f "$CONF/allowed_signers" ]; then
  git_ -c gpg.format=ssh -c gpg.ssh.allowedSignersFile="$CONF/allowed_signers" \
    verify-commit "$NEW" >/dev/null 2>&1 || fail "not signed by an allowed key"
fi

log "updating ${CURRENT:0:7} -> $SHORT: $(git_ log -1 --format=%s "$NEW")"
git_ checkout --quiet --force "$NEW"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null || fail "configure failed"
cmake --build build -j2 >/dev/null || fail "build failed"
(cd build && ctest --output-on-failure >/dev/null) || fail "tests failed"

# Swap the binary atomically, keeping the old one for a rollback.
cp -p "$BIN" "$STATE/wallet-fw.previous" 2>/dev/null || true
install -m 0755 build/wallet-fw "$BIN.new"
mv -f "$BIN.new" "$BIN"
if [ -d "$(dirname "$SERVICE")" ] && ! cmp -s deploy/wallet-fw.service "$SERVICE"; then
  install -m 0644 deploy/wallet-fw.service "$SERVICE"
  $SYSTEMCTL daemon-reload
fi
$SYSTEMCTL restart wallet-fw
sleep "$HEALTH_WAIT"
if $SYSTEMCTL is-active --quiet wallet-fw &&
   [ "$($SYSTEMCTL show -p NRestarts --value wallet-fw)" = 0 ]; then
  echo "$NEW" > "$STATE/installed"
  # The updater and its timer update themselves; used from the next run.
  install -m 0755 scripts/aurora-update.sh /usr/local/sbin/aurora-update 2>/dev/null || true
  log "installed $SHORT"
  exit 0
fi
if [ -f "$STATE/wallet-fw.previous" ]; then
  mv -f "$STATE/wallet-fw.previous" "$BIN"
  $SYSTEMCTL restart wallet-fw
fi
fail "wallet-fw did not stay running after the update"
