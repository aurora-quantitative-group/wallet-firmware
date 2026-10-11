#!/usr/bin/env bash
# One-time setup of Aurora's automatic updates from GitHub. Afterwards the Pi
# checks the firmware branch every 15 minutes and installs new commits by
# itself (see scripts/aurora-update.sh); no SSH or copying needed.
#
#   sudo scripts/install-updater.sh           # read-only deploy key (recommended)
#   sudo scripts/install-updater.sh --token   # or a read-only GitHub token
#
# A public repository needs neither. Run it after scripts/setup-pi.sh.
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CONF=/etc/aurora-update
REPO=blockchain-wallet-hackathon/wallet-firmware
HTTPS_URL="https://github.com/$REPO.git"
SSH_URL="git@github.com:$REPO.git"
BRANCH="${AURORA_BRANCH:-firmware}"

die() { echo "error: $*" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || die "run with sudo"
command -v git >/dev/null || apt-get install -y git

install -d -m 0700 "$CONF"
install -m 0755 "$SRC_DIR/scripts/aurora-update.sh" /usr/local/sbin/aurora-update
install -m 0644 "$SRC_DIR/deploy/aurora-update.service" "$SRC_DIR/deploy/aurora-update.timer" \
  /etc/systemd/system/

if GIT_TERMINAL_PROMPT=0 git -c credential.helper= ls-remote "$HTTPS_URL" >/dev/null 2>&1; then
  echo "The repository is public: no key needed."
  rm -f "$CONF/token" "$CONF/deploy_key" "$CONF/deploy_key.pub"
  URL=$HTTPS_URL
elif [ "${1:-}" = --token ]; then
  echo "Create a fine-grained token on GitHub (Settings > Developer settings > Personal"
  echo "access tokens) for $REPO with Contents: Read-only, and paste it here."
  read -r -s -p "Token: " token
  echo
  [ -n "$token" ] || die "no token entered"
  (umask 077 && printf '%s' "$token" > "$CONF/token")
  rm -f "$CONF/deploy_key" "$CONF/deploy_key.pub"
  URL=$HTTPS_URL
else
  rm -f "$CONF/token"
  [ -f "$CONF/deploy_key" ] ||
    ssh-keygen -q -t ed25519 -N "" -C "aurora-update@$(hostname)" -f "$CONF/deploy_key"
  echo
  echo "Add this key on GitHub: github.com/$REPO > Settings > Deploy keys > Add deploy key."
  echo "Title: Aurora $(hostname). Leave \"Allow write access\" OFF."
  echo
  cat "$CONF/deploy_key.pub"
  echo
  read -r -p "Press Enter once the key is added... "
  URL=$SSH_URL
fi
printf 'REPO_URL=%s\nBRANCH=%s\n' "$URL" "$BRANCH" > "$CONF/update.conf"
chmod 0600 "$CONF/update.conf"

/usr/local/sbin/aurora-update --check ||
  die "GitHub refused the key or token; check it and run this script again"

systemctl daemon-reload
systemctl enable --now aurora-update.timer
systemctl start --no-block aurora-update.service
cat <<EOF

Automatic updates are on: the Pi checks $BRANCH every 15 minutes, builds and
tests new commits, installs them and restarts wallet-fw (rolling back if it
will not start). The first check is running now and takes a few minutes.

  journalctl -u aurora-update -f            # watch it
  sudo systemctl start aurora-update        # check right now
  sudo systemctl disable --now aurora-update.timer   # turn updates off
EOF
