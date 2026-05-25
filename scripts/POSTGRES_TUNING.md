# PostgreSQL Write Cache Tuning

This directory contains scripts to optimize PostgreSQL for blockchain indexing workloads.

## Problem

During blockchain synchronization, PostgreSQL was holding write cache in memory, and the OS page cache grew to 42GB while PostgreSQL memory stayed low. This meant write operations were being buffered too aggressively.

## Solution

Optimize PostgreSQL to flush cache to disk more frequently, reducing memory pressure during write-heavy operations.

## Usage

### Apply Tuning

```bash
sudo ./tune_postgres.sh
```

This script will:
1. Backup the current PostgreSQL configuration
2. Update checkpoint and background writer settings
3. Reload PostgreSQL (no restart required)
4. Verify settings are active

### Revert to Defaults

```bash
sudo ./revert_postgres_tuning.sh
```

This script will:
1. Find the most recent backup
2. Restore the configuration
3. Reload PostgreSQL

## Settings Changed

| Setting | Before | After | Effect |
|---------|--------|-------|--------|
| `checkpoint_timeout` | 15min | 5min | Checkpoint every 5 min instead of 15 |
| `checkpoint_completion_target` | 0.9 | 0.5 | Spread I/O over shorter period |
| `bgwriter_delay` | 200ms | 100ms | Background writer runs 2x more often |
| `bgwriter_lru_maxpages` | 100 | 500 | Flush 5x more pages per cycle |
| `bgwriter_lru_multiplier` | 2.0 | 3.0 | Scan buffers more aggressively |

## Why These Changes Help

1. **More Frequent Checkpoints** (5min vs 15min)
   - Forces dirty pages to disk more often
   - Reduces peak cache memory usage
   - Prevents long pause before checkpoints

2. **Faster Checkpoint Completion** (0.5 vs 0.9)
   - Spreads I/O over 50% of interval instead of 90%
   - Reduces sudden disk load spikes

3. **More Active Background Writer**
   - Runs every 100ms instead of 200ms (2x more frequent)
   - Flushes more pages per cycle (5x more)
   - Aggressively evicts LRU pages

## Expected Impact

- **Memory Usage**: Reduced peak cache as writes flush more frequently
- **I/O Pattern**: More consistent writes instead of bursty checkpoints
- **Performance**: Slight I/O increase, but better for write-heavy workloads
- **Responsiveness**: System more responsive during heavy indexing

## Backups

Configuration backups are automatically created with timestamps:
```
/etc/postgresql/16/main/postgresql.conf.backup.20241222_143025
```

To manually revert:
```bash
sudo cp /etc/postgresql/16/main/postgresql.conf.backup.TIMESTAMP /etc/postgresql/16/main/postgresql.conf
sudo systemctl reload postgresql
```

## Verification

Check current settings:
```bash
sudo -u postgres psql -c "\
  SELECT
    current_setting('checkpoint_timeout') as checkpoint_timeout,
    current_setting('checkpoint_completion_target') as checkpoint_completion_target,
    current_setting('bgwriter_delay') as bgwriter_delay,
    current_setting('bgwriter_lru_maxpages') as bgwriter_lru_maxpages,
    current_setting('bgwriter_lru_multiplier') as bgwriter_lru_multiplier
"
```

Monitor background writer activity:
```bash
sudo -u postgres psql -c "SELECT * FROM pg_stat_bgwriter"
```

## References

- [PostgreSQL Checkpoint Configuration](https://www.postgresql.org/docs/current/wal-configuration.html)
- [PostgreSQL Background Writer](https://www.postgresql.org/docs/current/runtime-config-resource.html#RUNTIME-CONFIG-RESOURCE-BACKGROUND-WRITER)
