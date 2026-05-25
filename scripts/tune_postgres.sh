#!/bin/bash

# PostgreSQL Write Cache Tuning Script
# Optimizes PostgreSQL for blockchain indexing workload
# Increases checkpoint frequency and background writer activity

set -e

CONFIG_FILE="/etc/postgresql/16/main/postgresql.conf"
BACKUP_FILE="${CONFIG_FILE}.backup.$(date +%Y%m%d_%H%M%S)"

echo "PostgreSQL Write Cache Tuning Script"
echo "====================================="
echo ""

# Check if running as root or with sudo
if [[ $EUID -ne 0 ]]; then
   echo "Error: This script must be run as root or with sudo"
   exit 1
fi

# Check if PostgreSQL config exists
if [[ ! -f "$CONFIG_FILE" ]]; then
    echo "Error: PostgreSQL config not found at $CONFIG_FILE"
    exit 1
fi

echo "Step 1: Backing up PostgreSQL configuration..."
cp "$CONFIG_FILE" "$BACKUP_FILE"
echo "  ✓ Backup saved to: $BACKUP_FILE"
echo ""

echo "Step 2: Applying write cache tuning settings..."

# Update checkpoint_timeout from 15min to 5min
sed -i 's/^checkpoint_timeout = 15min/checkpoint_timeout = 5min/' "$CONFIG_FILE"
echo "  ✓ checkpoint_timeout: 15min → 5min"

# Update checkpoint_completion_target from 0.9 to 0.5
sed -i 's/^checkpoint_completion_target = 0.9/checkpoint_completion_target = 0.5/' "$CONFIG_FILE"
echo "  ✓ checkpoint_completion_target: 0.9 → 0.5"

# Update bgwriter_delay from 200ms to 100ms
sed -i 's/^bgwriter_delay = 200ms/bgwriter_delay = 100ms/' "$CONFIG_FILE"
echo "  ✓ bgwriter_delay: 200ms → 100ms"

# Update bgwriter_lru_maxpages from 100 to 500
sed -i 's/^bgwriter_lru_maxpages = 100/bgwriter_lru_maxpages = 500/' "$CONFIG_FILE"
echo "  ✓ bgwriter_lru_maxpages: 100 → 500"

# Update bgwriter_lru_multiplier from 2.0 to 3.0
sed -i 's/^bgwriter_lru_multiplier = 2.0/bgwriter_lru_multiplier = 3.0/' "$CONFIG_FILE"
echo "  ✓ bgwriter_lru_multiplier: 2.0 → 3.0"

echo ""
echo "Step 3: Verifying changes..."
echo ""
echo "Updated settings in $CONFIG_FILE:"
echo "---"
grep "checkpoint_timeout\|checkpoint_completion_target\|bgwriter_delay\|bgwriter_lru_maxpages\|bgwriter_lru_multiplier" "$CONFIG_FILE" | grep -v "^#" | sed 's/^/  /'
echo "---"
echo ""

echo "Step 4: Reloading PostgreSQL configuration..."
systemctl reload postgresql

if systemctl is-active --quiet postgresql; then
    echo "  ✓ PostgreSQL reloaded successfully"
else
    echo "  ✗ Error: PostgreSQL failed to reload"
    echo "  Restoring backup from: $BACKUP_FILE"
    cp "$BACKUP_FILE" "$CONFIG_FILE"
    systemctl reload postgresql
    exit 1
fi

echo ""
echo "Step 5: Verifying settings are active..."
SETTINGS=$(sudo -u postgres psql -t -c "
  SELECT
    current_setting('checkpoint_timeout') as checkpoint_timeout,
    current_setting('checkpoint_completion_target') as checkpoint_completion_target,
    current_setting('bgwriter_delay') as bgwriter_delay,
    current_setting('bgwriter_lru_maxpages') as bgwriter_lru_maxpages,
    current_setting('bgwriter_lru_multiplier') as bgwriter_lru_multiplier
" 2>/dev/null)

if [[ $? -eq 0 ]]; then
    echo "  ✓ Active PostgreSQL settings:"
    echo "$SETTINGS" | sed 's/^/    /'
else
    echo "  ✗ Could not verify settings (PostgreSQL may require restart)"
    echo ""
    echo "To restart PostgreSQL and apply changes:"
    echo "  sudo systemctl restart postgresql"
fi

echo ""
echo "====================================="
echo "PostgreSQL tuning complete!"
echo ""
echo "Summary of changes:"
echo "  • Checkpoint frequency: Every 5 minutes (was 15 min)"
echo "  • Checkpoint completion: 50% (was 90%)"
echo "  • Background writer: Runs every 100ms (was 200ms)"
echo "  • Background writer pages: 500 per cycle (was 100)"
echo "  • Background writer multiplier: 3.0 (was 2.0)"
echo ""
echo "These settings optimize PostgreSQL for write-heavy workloads"
echo "like blockchain indexing by flushing cache more frequently."
echo ""
echo "Backup saved to: $BACKUP_FILE"
echo "To revert: sudo cp $BACKUP_FILE $CONFIG_FILE && sudo systemctl reload postgresql"
