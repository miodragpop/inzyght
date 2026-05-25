#!/usr/bin/env bash
# =============================================================================
# Inzyght — Drop database and role
#
# Usage:
#   ./drop_db.sh
#   DB_NAME=mydb DB_USER=myuser ./drop_db.sh
# =============================================================================

set -euo pipefail

DB_NAME="${DB_NAME:-inzyght}"
DB_USER="${DB_USER:-$(whoami)}"

echo "============================================="
echo "  Inzyght — Drop Database"
echo "============================================="
echo "  Database : $DB_NAME"
echo "  Role     : $DB_USER"
echo "============================================="
echo ""
read -rp "  This is irreversible. Type 'yes' to confirm: " CONFIRM
if [ "$CONFIRM" != "yes" ]; then
    echo "  Aborted."
    exit 0
fi

pg() {
    sudo -u postgres psql --no-password -v ON_ERROR_STOP=1 "$@"
}

# Terminate active connections before dropping
echo ""
echo "[1/2] Terminating active connections to '$DB_NAME'..."
pg -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = '$DB_NAME' AND pid <> pg_backend_pid();" 2>/dev/null || true

if pg -tAc "SELECT 1 FROM pg_database WHERE datname='$DB_NAME'" | grep -q 1; then
    pg -c "DROP DATABASE \"$DB_NAME\";"
    echo "      Database '$DB_NAME' dropped."
else
    echo "      Database '$DB_NAME' does not exist — skipping."
fi

echo ""
echo "[2/2] Dropping role '$DB_USER'..."
if pg -tAc "SELECT 1 FROM pg_roles WHERE rolname='$DB_USER'" | grep -q 1; then
    pg -c "DROP ROLE \"$DB_USER\";"
    echo "      Role '$DB_USER' dropped."
else
    echo "      Role '$DB_USER' does not exist — skipping."
fi

echo ""
echo "============================================="
echo "  Done."
echo "============================================="
echo ""
