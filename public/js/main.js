/**
 * Inzyght - Ycash Blockchain Explorer
 * Main JavaScript File
 */

// Configuration
const API_BASE = '/api/v1';
const REFRESH_INTERVAL = 5000; // 5 seconds
const MAX_SUPPLY_YEC = 21_000_000;
const BLOCK_TIME_TARGET = 75; // seconds, post-Blossom (Ycash activated at block 1,100,000)

// Utility Functions
function formatHash(hash, length = 32) {
    if (!hash) return '---';
    if (hash.length <= length) return hash;
    return hash.substring(0, length) + '...';
}

function formatNumber(num) {
    if (num === undefined || num === null) return '---';
    //return Number(num).toLocaleString();
    //return Number(num).toFixed();

    return new Intl.NumberFormat('fr-FR', {
    minimumFractionDigits: 0
    }).format(num).replace(/\s/g, '\u202F');
}

function formatDate(timestamp) {
    if (!timestamp) return '---';
    const seconds = Math.floor(Date.now() / 1000) - timestamp;
    if (seconds < 0) return 'just now';
    if (seconds < 60) return `${seconds} sec ago`;
    const minutes = Math.floor(seconds / 60);
    const remainingSecs = seconds % 60;
    if (minutes < 60) return `${minutes} min ${remainingSecs} sec ago`;
    const hours = Math.floor(minutes / 60);
    const remainingMins = minutes % 60;
    if (hours < 24) return `${hours} hr ${remainingMins} min ago`;
    const days = Math.floor(hours / 24);
    const remainingHours = hours % 24;
    return `${days} day ${remainingHours} hr ago`;
}

function formatBalance(balance) {
    if (!balance) return '0 YEC';
    return parseFloat(balance).toFixed(8) + ' YEC';
}

function formatDifficulty(difficulty) {
    if (!difficulty) return '---';
    if (difficulty > 1000000) {
        return (difficulty / 1000000).toFixed(2) + 'M';
    } else if (difficulty > 1000) {
        return (difficulty / 1000).toFixed(2) + 'K';
    }
    return difficulty.toFixed(2);
}

// Fetch network info from RPC via API
function fetchNetworkInfo() {
    $.ajax({
        url: API_BASE + '/network/info',
        method: 'GET',
        dataType: 'json',
        timeout: 5000,
        success: function(response) {
            if (response.status === 'success' && response.data) {
                const networkData = response.data;

                // Store chain tip height globally for ETA calculation
                chainTipHeight = networkData.blocks || 0;

                const data = {
                    blockHeight: networkData.blocks || 0,
                    difficulty: networkData.difficulty || 0,
                    connections: networkData.connections || 0,
                    protocolVersion: networkData.protocol_version || 0,
                    networkType: networkData.testnet ? 'testnet' : 'mainnet',
                    totalTransactions: networkData.total_transactions,
                    mempool_count: networkData.mempool_count,
                    networksolps: networkData.networksolps,
                    chainSupply: networkData.chain_supply,
                    transparentSupply: networkData.transparent_supply,
                    saplingSupply: networkData.sapling_supply,
                    sproutSupply: networkData.sprout_supply
                };

                updateStatistics(data);
            } else {
                console.warn('Invalid response from network info API');
            }
        },
        error: function(jqXHR, textStatus, errorThrown) {
            console.error('Error fetching network info:', textStatus, errorThrown);
            // Fall back — pass nulls so updateStatistics skips updating those fields
            const fallbackData = {
                blockHeight: null,
                difficulty: null,
                connections: null,
                protocolVersion: null,
                totalTransactions: null
            };
            updateStatistics(fallbackData);
        }
    });
}

// Update statistics on the page
function updateStatistics(data) {
    if (!data) return;

    // Handle both API format (blocks) and WebSocket format (blockHeight)
    const blockHeight = data.blockHeight !== undefined ? data.blockHeight : data.blocks;
    const difficulty = data.difficulty;
    const connections = data.connections;
    const protocolVersion = data.protocol_version !== undefined ? data.protocol_version : data.protocolVersion;
    const networkType = data.networkType || (data.testnet ? 'testnet' : 'mainnet');
    const totalTransactions = data.totalTransactions !== undefined ? data.totalTransactions : data.total_transactions;
    const mempoolCount = data.mempool_count;

    // Update stats cards (only update if we have the data)
    if (blockHeight !== undefined && blockHeight !== null) {
        $('#blockHeight').text(formatNumber(blockHeight));
        $('#blockHeightInfo').text(formatNumber(blockHeight));
    }

    if (totalTransactions != null) {
        $('#totalTx').text(formatNumber(totalTransactions));
    }

    if (mempoolCount != null) {
        $('#mempoolCount').text(formatNumber(mempoolCount));
        $('#mempoolBadgeWrap').toggle(mempoolCount > 0);
    }

    if (difficulty !== undefined && difficulty !== null) {
        $('#difficulty').text(formatDifficulty(difficulty));
        $('#difficultyInfo').text(formatDifficulty(difficulty));
    }

    if (connections !== undefined && connections !== null) {
        $('#connections').text(formatNumber(connections));
        $('#connectionsInfo').text(formatNumber(connections));
    }

    if (protocolVersion !== undefined) {
        $('#protocolVersion').text(protocolVersion);
    }

    if (networkType !== undefined) {
        $('#networkType').text(networkType);
    }

    if (data.networksolps != null) {
        $('#networksolps').text(formatDifficulty(data.networksolps));
    }

    if (data.chainSupply != null) {
        const total = data.chainSupply;
        const transparent = data.transparentSupply || 0;
        const sapling = data.saplingSupply || 0;
        const sprout = data.sproutSupply || 0;
        const fmt = v => Number(v).toFixed(8) + ' YEC';
        $('#chainSupply').text(fmt(total));
        $('#supplyPct').text((total / MAX_SUPPLY_YEC * 100).toFixed(2));
        $('#supplyTransparent').text(fmt(transparent));
        $('#supplySapling').text(fmt(sapling));
        $('#supplySprout').text(fmt(sprout));
        if (total > 0) {
            $('#poolTransparent').css('width', (transparent / total * 100).toFixed(2) + '%');
            $('#poolSapling').css('width', (sapling / total * 100).toFixed(2) + '%');
            $('#poolSprout').css('width', (sprout / total * 100).toFixed(2) + '%');
        }
    }
}

// Fetch latest blocks from API
function fetchLatestBlocks() {
    $.ajax({
        url: API_BASE + '/blocks',
        method: 'GET',
        dataType: 'json',
        timeout: 5000,
        success: function(response) {
            if (response.status === 'success' && response.data) {
                // Convert API response to display format
                const blocks = response.data.map(block => ({
                    height: block.height || 0,
                    hash: block.hash || '',
                    timestamp: block.time || 0,
                    tx_count: block.tx || 0,
                    miner: block.miner || 'Unknown',
                    size: block.size || 0,
                    reward: block.reward || 12.5
                }));
                displayLatestBlocks(blocks);
            } else {
                console.warn('Invalid response from blocks API');
                displayLatestBlocks([]);
            }
        },
        error: function(jqXHR, textStatus, errorThrown) {
            console.error('Error fetching blocks:', textStatus, errorThrown);
            // Display empty state
            displayLatestBlocks([]);
        }
    });
}

function computeAvgBlockTime(blocks) {
    if (!blocks || blocks.length < 2) return null;
    const sorted = [...blocks].sort((a, b) => a.height - b.height);
    const diffs = [];
    for (let i = 1; i < sorted.length; i++)
        diffs.push(sorted[i].timestamp - sorted[i - 1].timestamp);
    return diffs.reduce((a, b) => a + b, 0) / diffs.length;
}

// Display latest blocks in table
function displayLatestBlocks(blocks) {
    const tbody = $('#latestBlocksBody');
    tbody.empty();

    if (!blocks || blocks.length === 0) {
        tbody.html('<tr><td colspan="7" class="text-center text-muted">No blocks available</td></tr>');
        return;
    }

    const avgTime = computeAvgBlockTime(blocks);
    if (avgTime !== null)
        $('#avgBlockTime').text(avgTime.toFixed(1) + 's (target ' + BLOCK_TIME_TARGET + 's)');

    blocks.forEach(block => {
        const row = `
            <tr>
                <td>
                    <a href="/block/${block.height}" class="text-decoration-none">
                        <strong>${formatNumber(block.height)}</strong>
                    </a>
                </td>
                <td>
                    <code class="hash-cell">
                        <a href="/block/${block.hash}" class="text-decoration-none">${block.hash}</a>
                    </code>
                </td>
                <td>
                    <small class="text-muted block-time" data-timestamp="${block.timestamp}">${formatDate(block.timestamp)}</small>
                </td>
                <td class="text-center">
                    <span class="badge bg-info">${block.tx_count}</span>
                </td>
                <td>
                    <small>${block.miner}</small>
                </td>
                <td class="d-none d-xl-table-cell">
                    <small>${formatNumber(block.size)} B</small>
                </td>
                <td class="text-right">
                    <strong class="text-success">${formatBalance(block.reward)}</strong>
                </td>
            </tr>
        `;
        tbody.append(row);
    });
}

// Fetch latest transactions from API
function fetchLatestTransactions() {
    $.ajax({
        url: API_BASE + '/transactions',
        method: 'GET',
        dataType: 'json',
        timeout: 5000,
        success: function(response) {
            if (response.status === 'success' && response.data) {
                const transactions = response.data.map(tx => ({
                    hash: tx.hash || '',
                    blockheight: tx.blockheight || null
                }));
                displayLatestTransactions(transactions);
            } else {
                console.warn('Invalid response from transactions API');
                displayLatestTransactions([]);
            }
        },
        error: function(jqXHR, textStatus, errorThrown) {
            console.error('Error fetching transactions:', textStatus, errorThrown);
            // Display empty state
            displayLatestTransactions([]);
        }
    });
}

// Display latest transactions in table
function displayLatestTransactions(transactions) {
    const tbody = $('#latestTxBody');
    tbody.empty();

    if (!transactions || transactions.length === 0) {
        tbody.html('<tr><td colspan="2" class="text-center text-muted">No transactions available</td></tr>');
        return;
    }

    transactions.forEach(tx => {
        const status = tx.blockheight
            ? `<a href="/block/${tx.blockheight}" class="text-decoration-none"><span class="badge bg-success">Included in block ${formatNumber(tx.blockheight)}</span></a>`
            : '<span class="badge bg-warning text-dark">mempool</span>';

        const row = `
            <tr>
                <td>
                    <code class="hash-truncate" title="${tx.hash}">
                        <a href="/transaction/${tx.hash}" class="text-decoration-none">${formatHash(tx.hash, 64)}</a>
                    </code>
                </td>
                <td class="text-center">${status}</td>
            </tr>
        `;
        tbody.append(row);
    });
}

// ── Search ────────────────────────────────────────────────────────────────────

// Ycash address patterns:
//   Transparent P2PKH  s1…  (34 chars, base58)
//   Transparent P2SH   s3…  (34 chars, base58)
//   Sapling shielded   ys1… (78 chars, bech32)
// Also accept Zcash-style t1/t3/zs1 in case someone pastes one.
const ADDRESS_RE = /^(s[13]|t[13]|ys1|zs1)[a-zA-Z0-9]{20,}$/;
const HASH_RE    = /^[0-9a-fA-F]{64}$/;
const HEIGHT_RE  = /^\d{1,8}$/;     // 1–8 digit number (covers full Ycash chain)

function setupSearch() {
    $('#searchBtn').on('click', performSearch);
    $('#searchInput').on('keydown', function(e) {
        if (e.key === 'Enter') performSearch();
    });
    // Clear error styling on new input
    $('#searchInput').on('input', function() {
        $(this).removeClass('is-invalid');
        $('#search-feedback').html('<small class="text-muted">Block height, 64-char hash (block or transaction), or address</small>');
    });
}

function searchError(msg) {
    $('#searchInput').addClass('is-invalid');
    $('#search-feedback').html(`<small class="text-danger"><i class="fa fa-exclamation-circle"></i> ${msg}</small>`);
}

function searchBusy(on) {
    const btn = $('#searchBtn');
    if (on) {
        btn.prop('disabled', true).html('<i class="fa fa-spinner fa-spin"></i>');
    } else {
        btn.prop('disabled', false).html('<i class="fa fa-search"></i> Search');
    }
}

function performSearch() {
    const query = $('#searchInput').val().trim();
    if (!query) { searchError('Enter a block height, hash, or address.'); return; }

    // ── Block height ──────────────────────────────────────────────────────────
    if (HEIGHT_RE.test(query)) {
        window.location.href = '/block/' + query;
        return;
    }

    // ── Address ───────────────────────────────────────────────────────────────
    if (ADDRESS_RE.test(query)) {
        window.location.href = '/address/' + query;
        return;
    }

    // ── 64-char hex: block hash or transaction hash ───────────────────────────
    if (HASH_RE.test(query)) {
        resolveHash(query);
        return;
    }

    searchError('Unrecognized input. Enter a block height, 64-char hex hash, or address.');
}

// Try the hash as a block hash first; fall back to transaction.
function resolveHash(hash) {
    searchBusy(true);
    $.ajax({
        url: API_BASE + '/blocks/hash/' + hash,
        method: 'GET', dataType: 'json', timeout: 8000,
        success: function(r) {
            searchBusy(false);
            if (r.status === 'success') window.location.href = '/block/' + hash;
            else                        window.location.href = '/transaction/' + hash;
        },
        error: function() {
            searchBusy(false);
            window.location.href = '/transaction/' + hash;
        }
    });
}

// Format time duration (seconds to human readable)
function formatDuration(seconds) {
    if (!seconds || seconds <= 0) return '---';

    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const secs = Math.floor(seconds % 60);

    if (hours > 0) {
        return `${hours}h ${minutes}m`;
    } else if (minutes > 0) {
        return `${minutes}m ${secs}s`;
    } else {
        return `${secs}s`;
    }
}

// Fetch sync progress
function fetchSyncProgress() {
    $.ajax({
        url: API_BASE + '/sync/progress',
        method: 'GET',
        dataType: 'json',
        timeout: 5000,
        success: function(response) {
            if (response.status === 'success' && response.data) {
                if (response.data.blocks) {
                    updateSyncProgress(response.data.blocks);
                }
            }
        },
        error: function(xhr, status, error) {
            console.error('Failed to fetch sync progress:', error);
        }
    });
}

// Global variable to store current chain tip height
let chainTipHeight = 0;

// Update sync progress display
function updateSyncProgress(syncData) {
    const isSyncing = syncData.status === 'syncing';
    const percentage = Math.min(100, Math.max(0, parseFloat(syncData.percentage || 0)));

    if (isSyncing && percentage < 99.9) {
        // Show sync progress section
        $('#syncProgressSection').show();

        // Hide or show latest blocks/transactions based on sync status
        // (you might want to show them even during sync, or hide them)

        // Update progress bar
        $('#syncProgressBar').css('width', percentage + '%');
        $('#syncProgressBar').attr('aria-valuenow', percentage);
        $('#syncProgressText').text(percentage.toFixed(1) + '%');
        $('#syncPercentage').text(percentage.toFixed(1) + '%');

        // Update current and total heights
        const currentHeight = syncData.current_height || 0;
        $('#syncCurrentHeight').text(formatNumber(currentHeight));

        // Use chain tip height from network info API (stored in global variable)
        if (chainTipHeight > 0) {
            $('#syncTotalHeight').text(formatNumber(chainTipHeight));
        } else if (syncData.current_height && percentage > 0) {
            // Fallback to percentage-based calculation if chain tip not available yet
            const totalHeight = Math.floor(syncData.current_height / (percentage / 100));
            $('#syncTotalHeight').text(formatNumber(totalHeight));
        }

        // Update average sync speed (historical)
        const avgSpeed = syncData.avg_speed || 0;
        if (avgSpeed > 0) {
            $('#syncAvgSpeed').text(avgSpeed.toFixed(1) + ' blocks/sec');
        } else {
            $('#syncAvgSpeed').text('Calculating...');
        }

        // Update current batch speed (blocks in last batch / time to process)
        const currentSpeed = syncData.current_speed || 0;
        if (currentSpeed > 0) {
            $('#syncCurrentSpeed').text(currentSpeed.toFixed(1) + ' blocks/sec');
        } else {
            $('#syncCurrentSpeed').text('Calculating...');
        }

        // Update ETA - calculate based on chain tip height and average speed
        if (chainTipHeight > 0 && currentHeight > 0 && avgSpeed > 0) {
            const remainingBlocks = chainTipHeight - currentHeight;
            if (remainingBlocks > 0) {
                const etaSeconds = Math.floor(remainingBlocks / avgSpeed);
                $('#syncETA').text(formatDuration(etaSeconds));
            } else {
                $('#syncETA').text('Complete');
            }
        } else if (avgSpeed > 0) {
            $('#syncETA').text('Calculating...');
        } else {
            $('#syncETA').text('---');
        }
    } else {
        // Hide sync progress section when sync is complete
        $('#syncProgressSection').hide();
    }
}

// Polling interval ID for fallback mode (when WebSocket is disconnected)
let pollingInterval = null;

function startPolling() {
    if (pollingInterval) return;
    pollingInterval = setInterval(function() {
        fetchNetworkInfo();
        fetchLatestBlocks();
        fetchLatestTransactions();
    }, REFRESH_INTERVAL);
}

function stopPolling() {
    if (pollingInterval) {
        clearInterval(pollingInterval);
        pollingInterval = null;
    }
}

// Initialize page on document ready
$(document).ready(function() {
    // Setup search
    setupSearch();

    // Initial data load
    fetchNetworkInfo();
    fetchLatestBlocks();
    fetchLatestTransactions();
    fetchSyncProgress();

    // Sync progress always polls (frequent updates during sync, not pushed via WebSocket)
    setInterval(fetchSyncProgress, REFRESH_INTERVAL);

    // Refresh relative block times in-place without re-fetching
    setInterval(function() {
        $('.block-time[data-timestamp]').each(function() {
            $(this).text(formatDate(parseInt($(this).data('timestamp'))));
        });
    }, 3000);

    // Blocks/transactions/network are updated via WebSocket events.
    // Polling is used only as fallback when WebSocket is disconnected.
    // websocket-client.js calls startPolling()/stopPolling() on connect/disconnect.

    console.log('Inzyght initialized successfully');
});

// Global error handler
window.addEventListener('error', function(event) {
    console.error('Error:', event.error);
});
