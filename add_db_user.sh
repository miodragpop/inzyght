#!/usr/bin/env bash
# =============================================================================
# Inzyght — Add PostgreSQL user with full privileges on inzyght database
#
# Usage:
#   ./add_db_user.sh                        # prompts for username and password
#   DB_USER=myuser DB_PASS=mypass ./add_db_user.sh
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
DB_NAME="${DB_NAME:-inzyght}"

if [ -z "${DB_USER:-}" ]; then
    read -rp "New username: " DB_USER
fi

if [ -z "${DB_PASS:-}" ]; then
    read -rsp "Password for '$DB_USER': " DB_PASS
    echo
    read -rsp "Confirm password: " DB_PASS2
    echo
    if [ "$DB_PASS" != "$DB_PASS2" ]; then
        echo "Passwords do not match. Aborting."
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# Helper: run SQL as postgres superuser
# ---------------------------------------------------------------------------
pg() {
    sudo -u postgres psql --no-password -v ON_ERROR_STOP=1 "$@"
}

echo ""
echo "============================================="
echo "  Inzyght — Add Database User"
echo "============================================="
echo "  Database : $DB_NAME"
echo "  New user : $DB_USER"
echo "============================================="

# ---------------------------------------------------------------------------
# 1. Create role
# ---------------------------------------------------------------------------
echo ""
echo "[1/2] Creating role '$DB_USER'..."

if pg -tAc "SELECT 1 FROM pg_roles WHERE rolname='$DB_USER'" | grep -q 1; then
    echo "      Role '$DB_USER' already exists — updating password."
    pg -c "ALTER ROLE \"$DB_USER\" WITH LOGIN PASSWORD '$DB_PASS';"
else
    pg -c "CREATE ROLE \"$DB_USER\" WITH LOGIN PASSWORD '$DB_PASS';"
    echo "      Role '$DB_USER' created."
fi

# ---------------------------------------------------------------------------
# 2. Grant all privileges
# ---------------------------------------------------------------------------
echo ""
echo "[2/2] Granting all privileges on '$DB_NAME' to '$DB_USER'..."

pg -d "$DB_NAME" <<SQL
-- Database-level
GRANT ALL PRIVILEGES ON DATABASE "$DB_NAME" TO "$DB_USER";

-- Schema-level
GRANT ALL ON SCHEMA public TO "$DB_USER";

-- All existing tables, sequences, functions, procedures, views
GRANT ALL PRIVILEGES ON ALL TABLES     IN SCHEMA public TO "$DB_USER";
GRANT ALL PRIVILEGES ON ALL SEQUENCES  IN SCHEMA public TO "$DB_USER";
GRANT ALL PRIVILEGES ON ALL FUNCTIONS  IN SCHEMA public TO "$DB_USER";
GRANT ALL PRIVILEGES ON ALL ROUTINES   IN SCHEMA public TO "$DB_USER";

-- Default privileges for objects created in the future
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON TABLES     TO "$DB_USER";
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON SEQUENCES  TO "$DB_USER";
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON FUNCTIONS  TO "$DB_USER";
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT ALL ON ROUTINES   TO "$DB_USER";

-- Transfer ownership of all existing objects to the new user
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
             FROM pg_proc p
             JOIN pg_namespace n ON n.oid = p.pronamespace
             WHERE n.nspname = 'public' LOOP
        EXECUTE 'ALTER FUNCTION public.' || quote_ident(r.proname) || '(' || r.args || ') OWNER TO "$DB_USER"';
    END LOOP;
END;
\$\$;
SQL

echo "      Done."
echo ""
echo "============================================="
echo "  User '$DB_USER' is ready."
echo "  Add to inzyght.conf:"
echo "    [postgresql]"
echo "    username = $DB_USER"
echo "    password = $DB_PASS"
echo "============================================="
echo ""
