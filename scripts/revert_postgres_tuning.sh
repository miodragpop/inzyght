#!/bin/bash

# PostgreSQL Tuning Revert Script
# Reverts write cache settings to defaults

set -e

CONFIG_FILE="/etc/postgresql/16/main/postgresql.conf"
BACKUP_FILE="${CONFIG_FILE}.backup"

echo "PostgreSQL Tuning Revert Script"
echo "================================"
echo ""

# Check if running as root or with sudo
if [[ $EUID -ne 0 ]]; then
   echo "Error: This script must be run as root or with sudo"
   exit 1
fi

# Find the most recent backup
if [[ ! -f "$BACKUP_FILE"* ]]; then
    echo "Available backups:"
    ls -1 "${CONFIG_FILE}.backup"* 2>/dev/null || echo "No backups found"
    echo ""
    echo "To revert to defaults, restore your backup file:"
    echo "  sudo cp /etc/postgresql/16/main/postgresql.conf.backup.* $CONFIG_FILE"
    echo "  sudo systemctl reload postgresql"
    exit 1
fi

# Find the most recent backup
LATEST_BACKUP=$(ls -1t "${CONFIG_FILE}".backup.* 2>/dev/null | head -1)

if [[ -z "$LATEST_BACKUP" ]]; then
    echo "Error: No backup files found matching pattern: ${CONFIG_FILE}.backup.*"
    echo ""
    echo "Manual revert instructions:"
    echo "  1. Edit $CONFIG_FILE"
    echo "  2. Change these settings back to:"
    echo "     checkpoint_timeout = 15min"
    echo "     checkpoint_completion_target = 0.9"
    echo "     bgwriter_delay = 200ms"
    echo "     bgwriter_lru_maxpages = 100"
    echo "     bgwriter_lru_multiplier = 2.0"
    echo "  3. Run: sudo systemctl reload postgresql"
    exit 1
fi

echo "Found backup: $LATEST_BACKUP"
echo ""

read -p "Are you sure you want to revert to the backup? (yes/no): " -r
echo
if [[ ! $REPLY =~ ^[Yy][Ee][Ss]$ ]]; then
    echo "Revert cancelled"
    exit 0
fi

echo "Restoring configuration from backup..."
cp "$LATEST_BACKUP" "$CONFIG_FILE"
echo "  ✓ Restored: $CONFIG_FILE"
echo ""

echo "Reloading PostgreSQL..."
systemctl reload postgresql

if systemctl is-active --quiet postgresql; then
    echo "  ✓ PostgreSQL reloaded successfully"
else
    echo "  ✗ Error: PostgreSQL failed to reload"
    exit 1
fi

echo ""
echo "Verification of reverted settings:"
sudo -u postgres psql -t -c "
  SELECT
    current_setting('checkpoint_timeout') as checkpoint_timeout,
    current_setting('checkpoint_completion_target') as checkpoint_completion_target,
    current_setting('bgwriter_delay') as bgwriter_delay,
    current_setting('bgwriter_lru_maxpages') as bgwriter_lru_maxpages,
    current_setting('bgwriter_lru_multiplier') as bgwriter_lru_multiplier
" 2>/dev/null | sed 's/^/  /'

echo ""
echo "PostgreSQL settings have been reverted to defaults"
