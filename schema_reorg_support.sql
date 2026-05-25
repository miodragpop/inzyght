-- ==============================================================================
-- REORG SUPPORT SCHEMA MIGRATIONS
-- ==============================================================================
-- This file adds tables and columns to support chain reorganization (reorg)
-- handling. Run this AFTER schema.sql to add reorg tracking capabilities.
--
-- Reorg events occur when the blockchain switches to a different chain.
-- This schema allows detecting, logging, and recovering from reorgs.
-- ==============================================================================

-- ==============================================================================
-- REORG EVENTS TABLE - Audit trail of all reorg events
-- ==============================================================================
CREATE TABLE IF NOT EXISTS reorg_events (
    id BIGSERIAL PRIMARY KEY,
    detected_at TIMESTAMP DEFAULT NOW(),
    reorg_height INT NOT NULL,                    -- Height where reorg occurred
    reorg_depth INT NOT NULL,                     -- How many blocks were affected
    old_block_hash VARCHAR(64),                   -- Hash of block before reorg
    new_block_hash VARCHAR(64),                   -- Hash of block after reorg
    affected_tx_count INT DEFAULT 0,              -- Number of transactions affected
    affected_addresses INT DEFAULT 0,             -- Number of addresses affected
    rollback_duration_ms BIGINT,                  -- How long rollback took
    status VARCHAR(50) DEFAULT 'detected',        -- 'detected', 'rolling_back', 'complete', 'failed'
    error_message TEXT,                           -- Error details if status='failed'
    completed_at TIMESTAMP                        -- When reorg was fully resolved
);

-- Indexes for efficient querying of reorg events
CREATE INDEX idx_reorg_detected_at ON reorg_events(detected_at DESC);
CREATE INDEX idx_reorg_status ON reorg_events(status) WHERE status != 'complete';
CREATE INDEX idx_reorg_height ON reorg_events(reorg_height DESC);

-- ==============================================================================
-- ADD REORG TRACKING TO SYNC_PROGRESS TABLE
-- ==============================================================================
ALTER TABLE sync_progress ADD COLUMN IF NOT EXISTS last_reorg_height INT;
ALTER TABLE sync_progress ADD COLUMN IF NOT EXISTS last_reorg_time TIMESTAMP;
ALTER TABLE sync_progress ADD COLUMN IF NOT EXISTS last_reorg_depth INT DEFAULT 0;
ALTER TABLE sync_progress ADD COLUMN IF NOT EXISTS consecutive_reorg_count INT DEFAULT 0;

-- Comments for new columns
COMMENT ON COLUMN sync_progress.last_reorg_height IS 'Height where last reorg was detected (for audit trail)';
COMMENT ON COLUMN sync_progress.last_reorg_time IS 'When the last reorg was detected';
COMMENT ON COLUMN sync_progress.last_reorg_depth IS 'Depth (in blocks) of last reorg';
COMMENT ON COLUMN sync_progress.consecutive_reorg_count IS 'Count of reorgs in last 24 hours (anomaly detection)';

-- ==============================================================================
-- ADD VERIFICATION TRACKING TO BLOCKS TABLE
-- ==============================================================================
-- Tracks which blocks have been "finalized" (unlikely to be affected by reorg)
-- A block is finalized after ~100 blocks have been added after it (or configurable depth)
ALTER TABLE blocks ADD COLUMN IF NOT EXISTS finality_depth INT DEFAULT 0;
ALTER TABLE blocks ADD COLUMN IF NOT EXISTS is_finalized BOOLEAN DEFAULT FALSE;
ALTER TABLE blocks ADD COLUMN IF NOT EXISTS finalized_at TIMESTAMP;

-- Comments for new columns
COMMENT ON COLUMN blocks.finality_depth IS 'Number of blocks after this one when marked as finalized';
COMMENT ON COLUMN blocks.is_finalized IS 'True if block is beyond reorg risk (usually >100 blocks deep)';
COMMENT ON COLUMN blocks.finalized_at IS 'Timestamp when block was marked as finalized';

-- Index for quickly finding non-finalized blocks (candidates for reorg rollback)
CREATE INDEX idx_blocks_finalized ON blocks(is_finalized) WHERE is_finalized = FALSE;

-- ==============================================================================
-- ADD REORG EVENT TRACKING TO TRANSACTIONS TABLE
-- ==============================================================================
ALTER TABLE transactions ADD COLUMN IF NOT EXISTS affected_by_reorg_count INT DEFAULT 0;
ALTER TABLE transactions ADD COLUMN IF NOT EXISTS last_reorg_at TIMESTAMP;

-- Comments for new columns
COMMENT ON COLUMN transactions.affected_by_reorg_count IS 'How many times this tx was rolled back and re-indexed due to reorgs';
COMMENT ON COLUMN transactions.last_reorg_at IS 'Timestamp of last reorg that affected this transaction';

-- ==============================================================================
-- HELPER FUNCTION: Update reorg tracking
-- ==============================================================================
CREATE OR REPLACE FUNCTION record_reorg_event(
    p_reorg_height INT,
    p_reorg_depth INT,
    p_old_hash VARCHAR,
    p_new_hash VARCHAR,
    p_affected_tx INT,
    p_affected_addresses INT,
    p_duration_ms BIGINT
) RETURNS void AS $$
BEGIN
    -- Insert reorg event
    INSERT INTO reorg_events (
        reorg_height,
        reorg_depth,
        old_block_hash,
        new_block_hash,
        affected_tx_count,
        affected_addresses,
        rollback_duration_ms,
        status
    ) VALUES (
        p_reorg_height,
        p_reorg_depth,
        p_old_hash,
        p_new_hash,
        p_affected_tx,
        p_affected_addresses,
        p_duration_ms,
        'complete'
    );

    -- Update sync_progress with reorg info
    UPDATE sync_progress SET
        last_reorg_height = p_reorg_height,
        last_reorg_time = NOW(),
        last_reorg_depth = p_reorg_depth,
        consecutive_reorg_count = CASE
            -- Reset counter if >24 hours since last reorg
            WHEN EXTRACT(EPOCH FROM (NOW() - last_reorg_time)) > 86400 THEN 1
            ELSE consecutive_reorg_count + 1
        END,
        updated_at = NOW()
    WHERE name = 'blocks';
END;
$$ LANGUAGE plpgsql;

-- ==============================================================================
-- HELPER FUNCTION: Mark blocks as finalized
-- ==============================================================================
CREATE OR REPLACE FUNCTION finalize_blocks(
    p_finality_depth INT DEFAULT 100
) RETURNS INT AS $$
DECLARE
    v_finalized_count INT;
    v_current_height INT;
BEGIN
    -- Get current blockchain height
    SELECT COALESCE(total_height, 0) INTO v_current_height FROM sync_progress WHERE name = 'blocks';

    -- Mark blocks as finalized if they're more than p_finality_depth blocks behind the tip
    UPDATE blocks SET
        is_finalized = TRUE,
        finality_depth = v_current_height - height,
        finalized_at = NOW()
    WHERE is_finalized = FALSE
      AND height <= (v_current_height - p_finality_depth);

    -- Return count of newly finalized blocks
    GET DIAGNOSTICS v_finalized_count = ROW_COUNT;
    RETURN v_finalized_count;
END;
$$ LANGUAGE plpgsql;

-- ==============================================================================
-- HELPER FUNCTION: Rollback after reorg
-- ==============================================================================
CREATE OR REPLACE FUNCTION rollback_reorg(
    p_target_height INT
) RETURNS TABLE(blocks_deleted INT, txs_deleted INT, success BOOLEAN) AS $$
DECLARE
    v_blocks_deleted INT;
    v_txs_deleted INT;
BEGIN
    BEGIN
        -- Start transaction
        -- Delete address_transactions (join table, no cascade issues)
        DELETE FROM address_transactions
        WHERE transaction_id IN (
            SELECT id FROM transactions WHERE block_height > p_target_height
        );

        -- Delete transaction inputs
        DELETE FROM transaction_inputs
        WHERE transaction_id IN (
            SELECT id FROM transactions WHERE block_height > p_target_height
        );

        -- Delete transaction outputs
        DELETE FROM transaction_outputs
        WHERE transaction_id IN (
            SELECT id FROM transactions WHERE block_height > p_target_height
        );

        -- Count and delete transactions
        WITH deleted_txs AS (
            DELETE FROM transactions WHERE block_height > p_target_height
            RETURNING id
        )
        SELECT COUNT(*) INTO v_txs_deleted FROM deleted_txs;

        -- Count and delete blocks
        WITH deleted_blocks AS (
            DELETE FROM blocks WHERE height > p_target_height
            RETURNING id
        )
        SELECT COUNT(*) INTO v_blocks_deleted FROM deleted_blocks;

        -- Return results
        RETURN QUERY SELECT v_blocks_deleted, v_txs_deleted, TRUE;

    EXCEPTION WHEN OTHERS THEN
        -- On error, return failure
        RETURN QUERY SELECT 0::INT, 0::INT, FALSE;
    END;
END;
$$ LANGUAGE plpgsql;

-- ==============================================================================
-- HELPER FUNCTION: Get recent reorg events
-- ==============================================================================
CREATE OR REPLACE FUNCTION get_recent_reorg_events(
    p_limit INT DEFAULT 10
) RETURNS TABLE(
    event_id BIGINT,
    detected_at TIMESTAMP,
    height INT,
    depth INT,
    tx_count INT,
    duration_ms BIGINT,
    status VARCHAR
) AS $$
BEGIN
    RETURN QUERY
    SELECT
        id,
        detected_at,
        reorg_height,
        reorg_depth,
        affected_tx_count,
        rollback_duration_ms,
        status
    FROM reorg_events
    ORDER BY detected_at DESC
    LIMIT p_limit;
END;
$$ LANGUAGE plpgsql;

-- ==============================================================================
-- HELPER FUNCTION: Detect if reorg occurred
-- ==============================================================================
CREATE OR REPLACE FUNCTION check_reorg_occurred(
    p_height INT,
    p_expected_hash VARCHAR
) RETURNS BOOLEAN AS $$
DECLARE
    v_db_hash VARCHAR;
BEGIN
    SELECT hash INTO v_db_hash FROM blocks WHERE height = p_height LIMIT 1;

    -- If hashes don't match, reorg occurred
    IF v_db_hash IS NOT NULL AND v_db_hash != p_expected_hash THEN
        RETURN TRUE;
    END IF;

    RETURN FALSE;
END;
$$ LANGUAGE plpgsql;

-- ==============================================================================
-- VIEWS FOR MONITORING REORG STATUS
-- ==============================================================================

-- Current reorg status
CREATE OR REPLACE VIEW reorg_status AS
SELECT
    sp.last_reorg_height,
    sp.last_reorg_time,
    sp.last_reorg_depth,
    sp.consecutive_reorg_count,
    (SELECT COUNT(*) FROM reorg_events WHERE status = 'complete') as total_reorgs,
    (SELECT AVG(reorg_depth) FROM reorg_events WHERE status = 'complete') as avg_reorg_depth,
    (SELECT MAX(reorg_depth) FROM reorg_events WHERE status = 'complete') as max_reorg_depth,
    CASE
        WHEN sp.consecutive_reorg_count > 2 AND EXTRACT(EPOCH FROM (NOW() - sp.last_reorg_time)) < 600
            THEN 'ANOMALY_DETECTED'
        WHEN sp.last_reorg_time IS NOT NULL
            THEN 'REORG_HISTORY'
        ELSE 'NORMAL'
    END as reorg_health
FROM sync_progress sp
WHERE sp.name = 'blocks';

-- Finalization progress
CREATE OR REPLACE VIEW finalization_progress AS
SELECT
    COUNT(*) FILTER (WHERE is_finalized = TRUE) as finalized_blocks,
    COUNT(*) FILTER (WHERE is_finalized = FALSE) as non_finalized_blocks,
    COUNT(*) as total_blocks,
    ROUND(100.0 * COUNT(*) FILTER (WHERE is_finalized = TRUE) / NULLIF(COUNT(*), 0), 2) as finalized_percentage
FROM blocks;

-- ==============================================================================
-- INITIAL DATA SETUP
-- ==============================================================================

-- Initialize reorg tracking in sync_progress (if not exists)
UPDATE sync_progress SET
    last_reorg_height = NULL,
    last_reorg_time = NULL,
    last_reorg_depth = 0,
    consecutive_reorg_count = 0
WHERE name = 'blocks' AND last_reorg_height IS NULL;

-- ==============================================================================
-- COMMENTS FOR DOCUMENTATION
-- ==============================================================================

COMMENT ON TABLE reorg_events IS 'Complete audit trail of all chain reorganization events';
COMMENT ON TABLE sync_progress IS 'Now includes reorg tracking for anomaly detection';
COMMENT ON VIEW reorg_status IS 'Current chain reorganization status and health';
COMMENT ON VIEW finalization_progress IS 'Progress of blocks reaching finalization depth';
