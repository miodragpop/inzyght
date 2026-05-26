-- =============================================================================
-- Inzyght — PostgreSQL Schema
-- Generated from live database via pg_dump
-- =============================================================================

SET statement_timeout = 0;
SET lock_timeout = 0;
SET client_encoding = 'UTF8';
SET standard_conforming_strings = on;
SET check_function_bodies = false;
SET xmloption = content;
SET client_min_messages = warning;
SET row_security = off;

-- =============================================================================
-- FUNCTIONS
-- =============================================================================

CREATE FUNCTION public.check_reorg_occurred(p_height integer, p_expected_hash character varying)
RETURNS boolean
LANGUAGE plpgsql
AS $$
DECLARE
    v_db_hash VARCHAR;
BEGIN
    SELECT hash INTO v_db_hash FROM blocks WHERE height = p_height LIMIT 1;
    IF v_db_hash IS NOT NULL AND v_db_hash != p_expected_hash THEN
        RETURN TRUE;
    END IF;
    RETURN FALSE;
END;
$$;

CREATE FUNCTION public.complete_sync(p_name character varying)
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
    UPDATE sync_progress
    SET status = 'completed', updated_at = NOW(), completed_at = NOW()
    WHERE name = p_name;
END;
$$;

CREATE FUNCTION public.get_recent_reorg_events(p_limit integer DEFAULT 10)
RETURNS TABLE(event_id bigint, detected_at timestamp without time zone, height integer, depth integer, tx_count integer, duration_ms bigint, status character varying)
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN QUERY
    SELECT id, detected_at, reorg_height, reorg_depth, affected_tx_count, rollback_duration_ms, status
    FROM reorg_events
    ORDER BY detected_at DESC
    LIMIT p_limit;
END;
$$;

CREATE FUNCTION public.get_sync_progress(p_name character varying)
RETURNS numeric
LANGUAGE plpgsql
AS $$
DECLARE
    v_progress NUMERIC;
BEGIN
    SELECT CASE
        WHEN total_height = 0 THEN 0
        ELSE ROUND((last_indexed_height::NUMERIC / total_height::NUMERIC) * 100, 2)
    END INTO v_progress
    FROM sync_progress
    WHERE name = p_name;
    RETURN COALESCE(v_progress, 0);
END;
$$;

CREATE FUNCTION public.record_reorg_event(p_reorg_height integer, p_reorg_depth integer, p_old_hash character varying, p_new_hash character varying, p_affected_tx integer, p_affected_addresses integer, p_duration_ms bigint)
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
    INSERT INTO reorg_events (
        reorg_height, reorg_depth, old_block_hash, new_block_hash,
        affected_tx_count, affected_addresses, rollback_duration_ms, status
    ) VALUES (
        p_reorg_height, p_reorg_depth, p_old_hash, p_new_hash,
        p_affected_tx, p_affected_addresses, p_duration_ms, 'complete'
    );
    UPDATE sync_progress SET
        last_reorg_height = p_reorg_height,
        last_reorg_time = NOW(),
        last_reorg_depth = p_reorg_depth,
        consecutive_reorg_count = CASE
            WHEN EXTRACT(EPOCH FROM (NOW() - last_reorg_time)) > 86400 THEN 1
            ELSE consecutive_reorg_count + 1
        END,
        updated_at = NOW()
    WHERE name = 'blocks';
END;
$$;

CREATE FUNCTION public.rollback_reorg(p_target_height integer)
RETURNS TABLE(blocks_deleted integer, txs_deleted integer, success boolean)
LANGUAGE plpgsql
AS $$
DECLARE
    v_blocks_deleted INT;
    v_txs_deleted INT;
BEGIN
    BEGIN
        DELETE FROM address_transactions
        WHERE transaction_id IN (
            SELECT id FROM transactions WHERE block_height > p_target_height
        );
        DELETE FROM transaction_inputs
        WHERE transaction_id IN (
            SELECT id FROM transactions WHERE block_height > p_target_height
        );
        DELETE FROM transaction_outputs
        WHERE transaction_id IN (
            SELECT id FROM transactions WHERE block_height > p_target_height
        );
        WITH deleted_txs AS (
            DELETE FROM transactions WHERE block_height > p_target_height RETURNING id
        )
        SELECT COUNT(*) INTO v_txs_deleted FROM deleted_txs;
        WITH deleted_blocks AS (
            DELETE FROM blocks WHERE height > p_target_height RETURNING height
        )
        SELECT COUNT(*) INTO v_blocks_deleted FROM deleted_blocks;
        RETURN QUERY SELECT v_blocks_deleted, v_txs_deleted, TRUE;
    EXCEPTION WHEN OTHERS THEN
        RETURN QUERY SELECT 0::INT, 0::INT, FALSE;
    END;
END;
$$;

CREATE FUNCTION public.update_sync_progress(p_name character varying, p_last_height integer, p_total_height integer, p_indexed_count bigint, p_status character varying, p_error_msg text DEFAULT NULL::text)
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
    INSERT INTO sync_progress (name, last_indexed_height, total_height, indexed_count, status, error_message, started_at, updated_at)
    VALUES (
        p_name, p_last_height, p_total_height, p_indexed_count, p_status, p_error_msg,
        CASE WHEN p_status = 'syncing' AND NOT EXISTS(
            SELECT 1 FROM sync_progress WHERE name = p_name AND status = 'syncing'
        ) THEN NOW() ELSE NULL END,
        NOW()
    )
    ON CONFLICT (name) DO UPDATE SET
        last_indexed_height = EXCLUDED.last_indexed_height,
        total_height        = EXCLUDED.total_height,
        indexed_count       = EXCLUDED.indexed_count,
        status              = EXCLUDED.status,
        error_message       = EXCLUDED.error_message,
        updated_at          = NOW(),
        completed_at        = CASE WHEN EXCLUDED.status = 'completed' THEN NOW() ELSE sync_progress.completed_at END;
    UPDATE sync_progress
    SET total_height = (SELECT MAX(id) FROM transactions)
    WHERE name = 'transactions';
END;
$$;

-- =============================================================================
-- SEQUENCES
-- =============================================================================

CREATE SEQUENCE public.reorg_events_id_seq  START WITH 1 INCREMENT BY 1 NO MINVALUE NO MAXVALUE CACHE 1;
CREATE SEQUENCE public.sync_progress_id_seq START WITH 1 INCREMENT BY 1 NO MINVALUE NO MAXVALUE CACHE 1;
CREATE SEQUENCE public.transactions_id_seq  START WITH 1 INCREMENT BY 1 NO MINVALUE NO MAXVALUE CACHE 1;

-- =============================================================================
-- TABLES
-- =============================================================================

CREATE TABLE public.blocks (
    height              integer NOT NULL,
    hash                bytea   NOT NULL,
    previous_hash       bytea,
    "timestamp"         bigint  NOT NULL,
    difficulty          double precision,
    size                integer,
    tx_count            integer,
    chain_supply        bigint,
    transparent_supply  bigint,
    sprout_supply       bigint,
    sapling_supply      bigint,
    miner_address       character varying(50),
    claimed_reward_block bigint,
    claimed_reward_miner bigint,
    CONSTRAINT blocks_pk PRIMARY KEY (height)
);


CREATE TABLE public.reorg_events (
    id                  bigint NOT NULL DEFAULT nextval('public.reorg_events_id_seq'),
    detected_at         timestamp without time zone DEFAULT now(),
    reorg_height        integer NOT NULL,
    reorg_depth         integer NOT NULL,
    old_block_hash      character varying(64),
    new_block_hash      character varying(64),
    affected_tx_count   integer DEFAULT 0,
    affected_addresses  integer DEFAULT 0,
    rollback_duration_ms bigint,
    status              character varying(50) DEFAULT 'detected',
    error_message       text,
    completed_at        timestamp without time zone,
    CONSTRAINT reorg_events_pkey PRIMARY KEY (id)
);

COMMENT ON TABLE public.reorg_events IS 'Complete audit trail of all chain reorganization events';

ALTER SEQUENCE public.reorg_events_id_seq OWNED BY public.reorg_events.id;


CREATE TABLE public.sync_progress (
    id                      integer NOT NULL DEFAULT nextval('public.sync_progress_id_seq'),
    name                    character varying(100) NOT NULL,
    last_indexed_height     integer,
    total_height            integer DEFAULT 0,
    indexed_count           bigint  DEFAULT 0,
    status                  character varying(50) DEFAULT 'idle',
    error_message           text,
    started_at              timestamp without time zone,
    updated_at              timestamp without time zone DEFAULT now(),
    completed_at            timestamp without time zone,
    last_reorg_height       integer,
    last_reorg_time         timestamp without time zone,
    last_reorg_depth        integer DEFAULT 0,
    consecutive_reorg_count integer DEFAULT 0,
    CONSTRAINT sync_progress_pkey     PRIMARY KEY (id),
    CONSTRAINT sync_progress_name_key UNIQUE (name)
);

COMMENT ON TABLE  public.sync_progress                        IS 'Now includes reorg tracking for anomaly detection';
COMMENT ON COLUMN public.sync_progress.status                 IS 'Current sync status: idle, syncing, paused, error, completed';
COMMENT ON COLUMN public.sync_progress.last_reorg_height      IS 'Height where last reorg was detected (for audit trail)';
COMMENT ON COLUMN public.sync_progress.last_reorg_time        IS 'When the last reorg was detected';
COMMENT ON COLUMN public.sync_progress.last_reorg_depth       IS 'Depth (in blocks) of last reorg';
COMMENT ON COLUMN public.sync_progress.consecutive_reorg_count IS 'Count of reorgs in last 24 hours (anomaly detection)';

ALTER SEQUENCE public.sync_progress_id_seq OWNED BY public.sync_progress.id;


CREATE TABLE public.transactions (
    id                      bigint  NOT NULL DEFAULT nextval('public.transactions_id_seq'),
    hash                    bytea   NOT NULL,
    block_height            integer NOT NULL,
    tx_index                integer,
    "timestamp"             bigint  NOT NULL,
    is_coinbase             boolean DEFAULT false,
    affected_by_reorg_count integer DEFAULT 0,
    last_reorg_at           timestamp without time zone,
    estimated_size          integer,
    CONSTRAINT transactions_pkey     PRIMARY KEY (id),
    CONSTRAINT transactions_hash_key UNIQUE (hash)
);

COMMENT ON TABLE  public.transactions                         IS 'All blockchain transactions with UTXO data';
COMMENT ON COLUMN public.transactions.affected_by_reorg_count IS 'How many times this tx was rolled back and re-indexed due to reorgs';
COMMENT ON COLUMN public.transactions.last_reorg_at           IS 'Timestamp of last reorg that affected this transaction';
COMMENT ON COLUMN public.transactions.estimated_size          IS 'Estimated transaction size (block size divided by number of transactions)';

ALTER SEQUENCE public.transactions_id_seq OWNED BY public.transactions.id;


-- =============================================================================
-- VIEWS
-- =============================================================================

CREATE VIEW public.reorg_status AS
SELECT
    last_reorg_height,
    last_reorg_time,
    last_reorg_depth,
    consecutive_reorg_count,
    (SELECT count(*) FROM public.reorg_events WHERE status::text = 'complete') AS total_reorgs,
    (SELECT avg(reorg_depth) FROM public.reorg_events WHERE status::text = 'complete') AS avg_reorg_depth,
    (SELECT max(reorg_depth) FROM public.reorg_events WHERE status::text = 'complete') AS max_reorg_depth,
    CASE
        WHEN consecutive_reorg_count > 2
             AND EXTRACT(epoch FROM now() - last_reorg_time::timestamp with time zone) < 600
        THEN 'ANOMALY_DETECTED'
        WHEN last_reorg_time IS NOT NULL THEN 'REORG_HISTORY'
        ELSE 'NORMAL'
    END AS reorg_health
FROM public.sync_progress sp
WHERE name::text = 'blocks';

COMMENT ON VIEW public.reorg_status IS 'Current chain reorganization status and health';

-- =============================================================================
-- INDEXES
-- =============================================================================

CREATE INDEX idx_blocks_hash      ON public.blocks       USING btree (hash);
CREATE INDEX idx_reorg_detected_at ON public.reorg_events USING btree (detected_at DESC);
CREATE INDEX idx_reorg_height     ON public.reorg_events USING btree (reorg_height DESC);
CREATE INDEX idx_reorg_status     ON public.reorg_events USING btree (status) WHERE status::text <> 'complete';
CREATE INDEX idx_sync_progress_name   ON public.sync_progress USING btree (name);
CREATE INDEX idx_sync_progress_status ON public.sync_progress USING btree (status);
CREATE INDEX idx_tx_block_height  ON public.transactions USING btree (block_height DESC);
CREATE INDEX idx_tx_is_coinbase   ON public.transactions USING btree (is_coinbase) WHERE is_coinbase = true;
