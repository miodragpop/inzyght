#!/bin/bash
# Clear all data from Inzyght database tables
# This script truncates all tables while preserving the schema

set -e

echo "========================================="
echo "Clear Inzyght Database Tables"
echo "========================================="
echo ""

# Database configuration — override via environment.
# DB_PASS must be set explicitly; no insecure default.
DB_NAME="${DB_NAME:-inzyght}"
DB_USER="${DB_USER:-ycash_user}"
DB_PASS="${DB_PASS?DB_PASS env var required: e.g. DB_PASS=mypassword ./clear_database.sh}"
DB_HOST="${DB_HOST:-127.0.0.1}"

# Warning prompt
echo "WARNING: This will delete all data from the following database:"
echo "  Database: $DB_NAME"
echo "  Host:     $DB_HOST"
echo ""
read -p "Are you sure you want to continue? (yes/NO): " -r
echo

if [[ ! $REPLY =~ ^[Yy][Ee][Ss]$ ]]; then
    echo "Operation cancelled."
    exit 0
fi

echo "Clearing all tables..."
echo ""

# Truncate all tables with CASCADE to handle foreign key constraints
PGPASSWORD="$DB_PASS" psql -U $DB_USER -d $DB_NAME -h $DB_HOST <<'EOF'
-- Truncate all tables (CASCADE handles foreign keys)
TRUNCATE TABLE blocks CASCADE;
TRUNCATE TABLE sync_progress CASCADE;
TRUNCATE TABLE reorg_events CASCADE;

-- Handle address_transactions if it exists (junction table)
DO $$
BEGIN
    IF EXISTS (SELECT FROM information_schema.tables WHERE table_name = 'address_transactions') THEN
        TRUNCATE TABLE address_transactions CASCADE;
    END IF;
END $$;

-- If shielded_pool_stats exists, truncate it
DO $$
BEGIN
    IF EXISTS (SELECT FROM information_schema.tables WHERE table_name = 'shielded_pool_stats') THEN
        TRUNCATE TABLE shielded_pool_stats CASCADE;
    END IF;
END $$;

-- TRUNCATE TABLE transaction_outputs CASCADE;
-- TRUNCATE TABLE transaction_inputs CASCADE;
TRUNCATE TABLE transactions CASCADE;

-- Reset sequences
-- ALTER SEQUENCE blocks_id_seq RESTART WITH 1;
ALTER SEQUENCE transactions_id_seq RESTART WITH 1;
-- ALTER SEQUENCE transaction_inputs_id_seq RESTART WITH 1;
-- ALTER SEQUENCE transaction_outputs_id_seq RESTART WITH 1;
ALTER SEQUENCE sync_progress_id_seq RESTART WITH 1;
ALTER SEQUENCE reorg_events_id_seq RESTART WITH 1;

-- Re-initialize sync_progress with default entry for blocks
INSERT INTO sync_progress (name, last_indexed_height, total_height, indexed_count, status)
VALUES ('headers', 0, 0, 0, 'idle'),
       ('blocks', 0, 0, 0, 'idle'),
       ('transactions', 0, 0, 0, 'idle');

-- Show table counts to verify
SELECT 'Blocks' as table_name, COUNT(*) as row_count FROM blocks
UNION ALL
SELECT 'Transactions', COUNT(*) FROM transactions
UNION ALL
SELECT 'Sync Progress', COUNT(*) FROM sync_progress
UNION ALL
SELECT 'Reorg Events', COUNT(*) FROM reorg_events;

EOF

echo ""
echo "========================================="
echo "Database Cleared Successfully"
echo "========================================="
echo ""
echo "All tables have been emptied and sequences reset."
echo "The schema structure remains intact."
echo ""
echo "You can now restart Inzyght to begin fresh indexing:"
echo "  cd build && ./inzyght"
echo ""
