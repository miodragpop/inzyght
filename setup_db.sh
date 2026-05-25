#!/usr/bin/env bash
# =============================================================================
# Inzyght — PostgreSQL database setup script
# Creates the database, user, and all required tables/indexes/functions.
#
# Usage:
#   ./setup_db.sh                        # uses defaults from inzyght.conf
#   DB_NAME=mydb DB_USER=myuser ./setup_db.sh
#   ./setup_db.sh --drop-existing        # drop and recreate if DB already exists
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration — override via environment variables if needed
# ---------------------------------------------------------------------------
DB_NAME="${DB_NAME:-inzyght}"
DB_USER="${DB_USER:-$(whoami)}"
# DB_PASS must be supplied via environment to avoid hardcoded credentials.
# Set to empty string explicitly to skip the password (peer/trust auth):
#   DB_PASS= ./setup_db.sh
DB_PASS="${DB_PASS?DB_PASS env var required (set to empty string to skip password / use peer auth)}"
DROP_EXISTING=false

for arg in "$@"; do
    case "$arg" in
        --drop-existing) DROP_EXISTING=true ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "============================================="
echo "  Inzyght — Database Setup"
echo "============================================="
echo "  Database : $DB_NAME"
echo "  User     : $DB_USER"
echo "  Host     : localhost (Unix socket)"
echo "============================================="

# ---------------------------------------------------------------------------
# Helper: run SQL as the postgres superuser
# ---------------------------------------------------------------------------
pg() {
    sudo -u postgres psql --no-password -v ON_ERROR_STOP=1 "$@"
}

# ---------------------------------------------------------------------------
# 1. Create role / user
# ---------------------------------------------------------------------------
echo ""
echo "[1/5] Creating database user '$DB_USER'..."

if pg -tAc "SELECT 1 FROM pg_roles WHERE rolname='$DB_USER'" | grep -q 1; then
    echo "      User '$DB_USER' already exists — skipping."
else
    if [ -n "$DB_PASS" ]; then
        pg -c "CREATE ROLE \"$DB_USER\" WITH LOGIN PASSWORD '$DB_PASS';"
    else
        pg -c "CREATE ROLE \"$DB_USER\" WITH LOGIN;"
    fi
    echo "      User '$DB_USER' created."
fi

# ---------------------------------------------------------------------------
# 2. Create database
# ---------------------------------------------------------------------------
echo ""
echo "[2/5] Creating database '$DB_NAME'..."

if pg -tAc "SELECT 1 FROM pg_database WHERE datname='$DB_NAME'" | grep -q 1; then
    if [ "$DROP_EXISTING" = true ]; then
        echo "      Dropping existing database '$DB_NAME'..."
        pg -c "DROP DATABASE \"$DB_NAME\";"
    else
        echo "      Database '$DB_NAME' already exists — skipping creation."
        echo "      (use --drop-existing to drop and recreate)"
    fi
fi

if ! pg -tAc "SELECT 1 FROM pg_database WHERE datname='$DB_NAME'" | grep -q 1; then
    pg -c "CREATE DATABASE \"$DB_NAME\" OWNER \"$DB_USER\" ENCODING 'UTF8';"
    echo "      Database '$DB_NAME' created."
fi

# Grant connect privilege
pg -c "GRANT ALL PRIVILEGES ON DATABASE \"$DB_NAME\" TO \"$DB_USER\";"
pg -d "$DB_NAME" -c "GRANT ALL ON SCHEMA public TO \"$DB_USER\";"

# ---------------------------------------------------------------------------
# 3. Apply main schema
# ---------------------------------------------------------------------------
echo ""
echo "[3/5] Applying schema.sql..."
SCHEMA_TMP=$(mktemp /tmp/inzyght_schema_XXXXXX.sql)
cp "$SCRIPT_DIR/schema.sql" "$SCHEMA_TMP"
chmod 644 "$SCHEMA_TMP"
pg -d "$DB_NAME" -f "$SCHEMA_TMP"
rm -f "$SCHEMA_TMP"
echo "      schema.sql applied."

# ---------------------------------------------------------------------------
# 4. Grant all privileges to DB_USER on everything in public schema
# ---------------------------------------------------------------------------
echo ""
echo "[4/5] Granting full privileges to '$DB_USER'..."
pg -d "$DB_NAME" <<SQL
-- Transfer ownership
DO \$\$
DECLARE r RECORD;
BEGIN
    FOR r IN SELECT tablename FROM pg_tables WHERE schemaname = 'public' LOOP
        EXECUTE 'ALTER TABLE public.' || quote_ident(r.tablename) || ' OWNER TO "$DB_USER"';
    END LOOP;
    FOR r IN SELECT sequence_name FROM information_schema.sequences WHERE sequence_schema = 'public' LOOP
        EXECUTE 'ALTER SEQUENCE public.' || quote_ident(r.sequence_name) || ' OWNER TO "$DB_USER"';
    END LOOP;
    FOR r IN SELECT viewname FROM pg_views WHERE schemaname = 'public' LOOP
        EXECUTE 'ALTER VIEW public.' || quote_ident(r.viewname) || ' OWNER TO "$DB_USER"';
    END LOOP;
    FOR r IN SELECT p.proname, pg_get_function_identity_arguments(p.oid) AS args
             FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
             WHERE n.nspname = 'public' LOOP
        EXECUTE 'ALTER FUNCTION public.' || quote_ident(r.proname) || '(' || r.args || ') OWNER TO "$DB_USER"';
    END LOOP;
END;
\$\$;
-- Grant all privileges
GRANT ALL PRIVILEGES ON ALL TABLES    IN SCHEMA public TO "$DB_USER";
GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA public TO "$DB_USER";
GRANT ALL PRIVILEGES ON ALL FUNCTIONS IN SCHEMA public TO "$DB_USER";
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES    TO "$DB_USER";
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON SEQUENCES TO "$DB_USER";
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON FUNCTIONS TO "$DB_USER";
SQL
echo "      Privileges granted."

# ---------------------------------------------------------------------------
# 5. Add user with full privileges (calls add_db_user.sh)
# ---------------------------------------------------------------------------
echo ""
echo "[5/5] Configuring user privileges via add_db_user.sh..."
DB_NAME="$DB_NAME" DB_USER="$DB_USER" DB_PASS="$DB_PASS" "$SCRIPT_DIR/add_db_user.sh"

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo "============================================="
echo "  Setup complete!"
echo "============================================="
echo ""
echo "  Make sure inzyght.conf contains:"
echo "    [postgresql]"
echo "    host     = 127.0.0.1"
echo "    port     = 5432"
echo "    database = $DB_NAME"
echo "    username = $DB_USER"
if [ -n "$DB_PASS" ]; then
echo "    password = $DB_PASS"
fi
echo ""
