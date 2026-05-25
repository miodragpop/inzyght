#!/usr/bin/env bash
# =============================================================================
# Inzyght — PostgreSQL performance tuning
# Optimized for write-heavy blockchain indexing on modest hardware (2-4 GB RAM)
#
# Usage:
#   ./pg_tune.sh            # assumes HDD
#   STORAGE=ssd ./pg_tune.sh
# =============================================================================

set -euo pipefail

STORAGE="${STORAGE:-hdd}"   # set to "ssd" if running on SSD/NVMe

PG_CONF=$(sudo -u postgres psql -tAc "SHOW config_file;")

echo "============================================="
echo "  PostgreSQL tuning for Inzyght"
echo "  Config : $PG_CONF"
echo "  Storage: $STORAGE"
echo "============================================="

# Helper: set or replace a parameter in postgresql.conf
pg_set() {
    local key="$1"
    local val="$2"
    if grep -qE "^#?${key}\s*=" "$PG_CONF"; then
        sudo sed -i "s|^#\?${key}\s*=.*|${key} = ${val}|" "$PG_CONF"
    else
        echo "${key} = ${val}" | sudo tee -a "$PG_CONF" > /dev/null
    fi
    echo "  $key = $val"
}

echo ""
echo "--- Memory ---"
# 25% of RAM is the standard recommendation; keep modest for small VPS
pg_set "shared_buffers"          "256MB"
# Per-operation sort/hash memory; low to avoid OOM with many connections
pg_set "work_mem"                "4MB"
# Used for VACUUM, CREATE INDEX — can spike higher temporarily
pg_set "maintenance_work_mem"    "64MB"
# Tells planner how much OS cache is available (doesn't allocate, just a hint)
pg_set "effective_cache_size"    "512MB"

echo ""
echo "--- WAL / Checkpoints ---"
# Don't wait for disk flush on each commit (biggest single perf win for indexing)
pg_set "synchronous_commit"      "off"
# Spread checkpoint I/O over 90% of the checkpoint interval (reduce I/O spikes)
pg_set "checkpoint_completion_target" "0.9"
# Allow WAL to grow up to 1GB before forcing a checkpoint
pg_set "max_wal_size"            "1GB"
pg_set "min_wal_size"            "128MB"
# Larger WAL buffer reduces WAL writes during bulk inserts
pg_set "wal_buffers"             "16MB"

echo ""
echo "--- Planner ---"
if [ "$STORAGE" = "ssd" ]; then
    # SSD: random reads are cheap, planner should prefer index scans
    pg_set "random_page_cost"        "1.1"
    pg_set "effective_io_concurrency" "200"
else
    # HDD: random reads are expensive
    pg_set "random_page_cost"        "4.0"
    pg_set "effective_io_concurrency" "2"
fi

echo ""
echo "--- Autovacuum (keep up with indexing inserts) ---"
pg_set "autovacuum_vacuum_scale_factor"  "0.05"
pg_set "autovacuum_analyze_scale_factor" "0.02"
pg_set "autovacuum_vacuum_cost_delay"    "2ms"

echo ""
echo "--- Connections ---"
# Inzyght uses its own connection pool; keep max_connections low to save memory
pg_set "max_connections"         "50"

# Reload PostgreSQL to apply (full restart needed only for shared_buffers change)
echo ""
echo "Restarting PostgreSQL to apply all settings..."
sudo systemctl restart postgresql

echo ""
echo "--- Verification ---"
sudo -u postgres psql -c "
SELECT name, setting, unit
FROM pg_settings
WHERE name IN (
    'synchronous_commit','shared_buffers','work_mem','maintenance_work_mem',
    'effective_cache_size','checkpoint_completion_target','max_wal_size',
    'min_wal_size','wal_buffers','random_page_cost','effective_io_concurrency',
    'autovacuum_vacuum_scale_factor','autovacuum_analyze_scale_factor',
    'autovacuum_vacuum_cost_delay','max_connections'
)
ORDER BY name;
"

echo ""
echo "============================================="
echo "  Done. Run with STORAGE=ssd for SSD/NVMe."
echo "============================================="
