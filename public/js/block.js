/**
 * Inzyght — Block detail page
 */

const API_BASE = '/api/v1';

// ── Helpers ──────────────────────────────────────────────────────────────────

function formatYec(value) {
    return Number(value).toFixed(8) + ' YEC';
}

function formatTimestamp(ts) {
    return new Date(ts * 1000).toLocaleString();
}

function formatNumber(n) {
    return Number(n).toLocaleString();
}

function truncateHash(hash, len = 24) {
    if (!hash) return '—';
    return hash.length > len ? hash.slice(0, len) + '…' : hash;
}

function showError(msg) {
    $('#loading').addClass('d-none');
    $('#error-msg').removeClass('d-none').text(msg);
}

// ── Identifier from URL ───────────────────────────────────────────────────────

function getBlockId() {
    // pathname is e.g. /block/123456  or  /block/abc123...
    const parts = window.location.pathname.split('/');
    return parts[parts.length - 1];
}

function isHeight(id) {
    return /^\d+$/.test(id);
}

// ── Fetch block ───────────────────────────────────────────────────────────────

function fetchBlock(id) {
    // Step 1: resolve hash if we got a height
    if (isHeight(id)) {
        $.ajax({
            url: API_BASE + '/blocks/' + id,
            method: 'GET', dataType: 'json', timeout: 10000,
            success: function(r) {
                if (r.status === 'success') fetchVerbose(r.data.hash);
                else showError('Block not found.');
            },
            error: function() { showError('Block not found.'); }
        });
    } else {
        fetchVerbose(id);
    }
}

function fetchVerbose(hash) {
    $.ajax({
        url: API_BASE + '/blocks/verbose/' + hash,
        method: 'GET', dataType: 'json', timeout: 30000,
        success: function(r) {
            if (r.status === 'success') renderBlock(r.data);
            else showError('Block not found.');
        },
        error: function() { showError('Failed to load block data.'); }
    });
}

// ── Render ────────────────────────────────────────────────────────────────────

function renderBlock(b) {
    document.title = 'Block ' + b.height + ' — Inzyght';
    initRawJsonModal(b.hash);

    // Nav arrows
    if (b.height > 0) {
        $('#prev-block').attr('href', '/block/' + (b.height - 1));
    } else {
        $('#prev-block').addClass('invisible');
    }
    if (b.next_hash) {
        $('#next-block').attr('href', '/block/' + b.next_hash);
    } else {
        $('#next-block').addClass('text-muted disabled');
    }

    // Title
    $('#hdr-height').text(formatNumber(b.height));

    // Header fields
    $('#hdr-hash').text(b.hash);
    $('#inf-height').text(formatNumber(b.height));
    $('#inf-time').text(formatTimestamp(b.time));
    $('#inf-confirmations').text(formatNumber(b.confirmations));
    $('#inf-size').text(formatNumber(b.size) + ' B');
    $('#inf-difficulty').text(Number(b.difficulty).toLocaleString(undefined, {maximumFractionDigits: 2}));
    $('#inf-bits').text(b.bits);
    $('#inf-version').text(b.version);
    $('#inf-tx-count').text(b.tx ? b.tx.length : 0);
    $('#inf-merkle').text(b.merkle_root);

    // Prev / Next block links
    if (b.previous_hash) {
        $('#inf-prev').attr('href', '/block/' + b.previous_hash).text(b.previous_hash);
    } else {
        $('#inf-prev').closest('.col-12').addClass('d-none');
    }
    if (b.next_hash) {
        $('#inf-next').attr('href', '/block/' + b.next_hash).text(b.next_hash);
    } else {
        $('#next-block-row').addClass('d-none');
    }

    // Miner
    if (b.miner_address && b.miner_address !== '') {
        const addr = b.miner_address;
        if (addr === 'private' || addr === 'unknown') {
            $('#inf-miner').removeAttr('href').text(addr);
        } else {
            $('#inf-miner').attr('href', '/address/' + addr).text(addr);
        }
    } else {
        $('#miner-row').addClass('d-none');
    }

    $('#inf-reward').text(formatYec(b.claimed_reward));
    $('#inf-fee').text(formatYec(b.block_fee / 1e8));

    // Value pools
    const poolNames = { transparent: 'Transparent', sprout: 'Sprout', sapling: 'Sapling' };
    const poolColors = { transparent: 'success', sprout: 'purple', sapling: 'info' };
    const poolContainer = $('#value-pools');
    (b.value_pools || []).forEach(pool => {
        const name = poolNames[pool.id] || pool.id;
        const color = poolColors[pool.id] || 'secondary';
        poolContainer.append(`
            <div class="col-md-4 col-6">
                <div class="p-2 bg-light rounded">
                    <small class="text-muted d-block">${name} Pool</small>
                    <strong class="text-${color}">${formatYec(pool.chain_value)}</strong>
                </div>
            </div>`);
    });

    // Transactions
    renderTransactions(b.tx || []);

    $('#loading').addClass('d-none');
    $('#block-content').removeClass('d-none');
    $('#tx-count-badge').text(b.tx ? b.tx.length : 0);
}

function txType(tx) {
    if (tx.vin && tx.vin.length > 0 && tx.vin[0].coinbase) return 'coinbase';
    if (tx.sapling_spends > 0 || tx.sapling_outputs > 0 || tx.joinsplits.length > 0) return 'shielded';
    return 'transparent';
}

function renderTransactions(txList) {
    const container = $('#tx-list');

    txList.forEach((tx, idx) => {
        const type = txType(tx);
        const isCoinbase = type === 'coinbase';

        // Fee: Σvin - Σvout + valueBalance(sapling) + Σvpub_new - Σvpub_old(sprout)
        let fee = null;
        if (!isCoinbase) {
            const sumVin  = (tx.vin  || []).reduce((s, i) => s + (i.value || 0), 0);
            const sumVout = (tx.vout || []).reduce((s, o) => s + (o.value || 0), 0);
            const sapling = tx.value_balance || 0;
            const sprout  = (tx.joinsplits || []).reduce((s, j) => s + (j.vpub_new || 0) - (j.vpub_old || 0), 0);
            fee = sumVin - sumVout + sapling + sprout;
        }

        // Type badges
        let badges = '';
        if (isCoinbase) badges += '<span class="badge badge-coinbase me-1">Coinbase</span>';
        if (tx.sapling_spends > 0 || tx.sapling_outputs > 0) {
            badges += `<span class="badge badge-sapling me-1">Sapling (${tx.sapling_spends} in / ${tx.sapling_outputs} out)</span>`;
        }
        if (tx.joinsplits && tx.joinsplits.length > 0) {
            badges += `<span class="badge badge-sprout me-1">Sprout (${tx.joinsplits.length} joinsplits)</span>`;
        }

        // Inputs
        let vinHtml = '';
        if (isCoinbase) {
            vinHtml = '<div class="io-row text-muted fst-italic">Coinbase (newly generated coins)</div>';
        } else {
            (tx.vin || []).forEach(inp => {
                const addr = inp.address
                    ? `<a href="/address/${inp.address}" class="font-monospace">${inp.address}</a>`
                    : `<span class="text-muted font-monospace">${truncateHash(inp.txid)}:${inp.vout}</span>`;
                vinHtml += `<div class="io-row d-flex justify-content-between">
                    <span>${addr}</span>
                    <span class="text-danger ms-3 text-nowrap">${inp.value ? '−' + formatYec(inp.value) : ''}</span>
                </div>`;
            });
            if (tx.sapling_spends > 0) {
                vinHtml += `<div class="io-row text-primary">${tx.sapling_spends} shielded Sapling input(s)</div>`;
            }
            if (tx.joinsplits && tx.joinsplits.length > 0) {
                tx.joinsplits.forEach(js => {
                    if (js.vpub_old > 0)
                        vinHtml += `<div class="io-row text-purple">Transparent → Sprout (shielding): ${formatYec(js.vpub_old)}</div>`;
                });
            }
        }

        // Outputs
        let voutHtml = '';
        (tx.vout || []).forEach(out => {
            const addrs = (out.addresses && out.addresses.length > 0)
                ? out.addresses.map(a => `<a href="/address/${a}" class="font-monospace">${a}</a>`).join(', ')
                : `<span class="text-muted">${out.type || 'nonstandard'}</span>`;
            const spentIndicator = out.spent_txid
                ? `<a href="/transaction/${out.spent_txid}" class="text-danger fw-bold ms-2 text-decoration-none" title="Spent in ${out.spent_txid}">S</a>`
                : `<span class="text-success fw-bold ms-2" title="Unspent">U</span>`;
            voutHtml += `<div class="io-row d-flex justify-content-between align-items-center">
                <span>${addrs}</span>
                <span class="d-flex align-items-center text-nowrap ms-3">
                    <span class="text-success">+${formatYec(out.value)}</span>${spentIndicator}
                </span>
            </div>`;
        });
        if (tx.sapling_outputs > 0) {
            const net = tx.value_balance;
            voutHtml += `<div class="io-row text-primary">${tx.sapling_outputs} shielded Sapling output(s)`;
            if (net !== 0) voutHtml += ` (net: ${formatYec(Math.abs(net))})`;
            voutHtml += `</div>`;
        }
        if (tx.joinsplits && tx.joinsplits.length > 0) {
            tx.joinsplits.forEach(js => {
                if (js.vpub_new > 0)
                    voutHtml += `<div class="io-row" style="color:#6f42c1">Sprout → transparent (unshielding): ${formatYec(js.vpub_new)}</div>`;
            });
        }

        const collapseId = 'tx-collapse-' + idx;

        container.append(`
            <div class="card border-0 shadow-sm mb-3 tx-card ${type}">
                <div class="card-header bg-transparent d-flex justify-content-between align-items-center py-2"
                     style="cursor:pointer" onclick="if(!event.target.closest('a')){bootstrap.Collapse.getOrCreateInstance(document.getElementById('${collapseId}')).toggle()}">
                    <span>
                        <a href="/transaction/${tx.txid}" class="font-monospace text-decoration-none">
                            ${tx.txid}
                        </a>
                    </span>
                    <span class="ms-2 d-flex align-items-center gap-2 text-nowrap">
                        ${badges}
                        ${fee !== null ? `<small class="text-muted">Fee: ${formatYec(fee)}</small>` : ''}
                    </span>
                </div>
                <div class="collapse show" id="${collapseId}">
                    <div class="card-body py-2">
                        <div class="row g-0">
                            <div class="col-md-6 pe-md-3 border-end-md">
                                <div class="text-muted small mb-1">Inputs (${isCoinbase ? 1 : (tx.vin || []).length + tx.sapling_spends + (tx.joinsplits || []).filter(j => j.vpub_old > 0).length})</div>
                                ${vinHtml || '<span class="text-muted small">—</span>'}
                            </div>
                            <div class="col-md-6 ps-md-3 mt-3 mt-md-0">
                                <div class="text-muted small mb-1">Outputs (${(tx.vout || []).length + tx.sapling_outputs + (tx.joinsplits || []).filter(j => j.vpub_new > 0).length})</div>
                                ${voutHtml || '<span class="text-muted small">—</span>'}
                            </div>
                        </div>
                    </div>
                </div>
            </div>`);
    });

    // Collapse all except first after render for blocks with many txs
    if (txList.length > 5) {
        $('#tx-list .collapse').not(':first').removeClass('show');
    }
}

// ── Copy hash ─────────────────────────────────────────────────────────────────

function copyHash() {
    const hash = $('#hdr-hash').text();
    navigator.clipboard.writeText(hash).then(() => {
        const btn = $('button[onclick="copyHash()"]');
        btn.html('<i class="fa fa-check"></i>');
        setTimeout(() => btn.html('<i class="fa fa-copy"></i>'), 2000);
    });
}

// ── Raw JSON modal ────────────────────────────────────────────────────────────

let rawJsonLoaded = false;

function initRawJsonModal(hash) {
    const modal = new bootstrap.Modal(document.getElementById('rawJsonModal'));

    $('#raw-json-btn').on('click', function() {
        modal.show();
        if (rawJsonLoaded) return;

        $.ajax({
            url: API_BASE + '/blocks/raw/' + hash,
            method: 'GET',
            dataType: 'text',
            timeout: 30000,
            success: function(text) {
                const pretty = JSON.stringify(JSON.parse(text), null, 2);
                const codeEl = document.getElementById('raw-json-code');
                codeEl.textContent = pretty;
                hljs.highlightElement(codeEl);
                $('#raw-json-loading').addClass('d-none');
                $('#raw-json-pre').removeClass('d-none');
                rawJsonLoaded = true;
            },
            error: function() {
                $('#raw-json-loading').text('Failed to load raw JSON.');
            }
        });
    });

    $('#raw-copy-btn').on('click', function() {
        const text = document.getElementById('raw-json-code').textContent;
        navigator.clipboard.writeText(text).then(() => {
            $('#raw-copy-btn').html('<i class="fa fa-check"></i> Copied!');
            setTimeout(() => $('#raw-copy-btn').html('<i class="fa fa-copy"></i> Copy'), 2000);
        });
    });
}

// ── Init ──────────────────────────────────────────────────────────────────────

$(document).ready(function() {
    const id = getBlockId();
    if (!id) {
        showError('No block identifier in URL.');
        return;
    }
    fetchBlock(id);
});
