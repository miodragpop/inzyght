/**
 * Inzyght — Transaction detail page
 */

const API_BASE = '/api/v1';

// ── Helpers ───────────────────────────────────────────────────────────────────

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

function getTxid() {
    const parts = window.location.pathname.split('/');
    return parts[parts.length - 1];
}

// ── Fetch ─────────────────────────────────────────────────────────────────────

function fetchTransaction(txid) {
    $.ajax({
        url: API_BASE + '/transactions/verbose/' + txid,
        method: 'GET', dataType: 'json', timeout: 15000,
        success: function(r) {
            if (r.status === 'success') renderTransaction(r.data);
            else showError('Transaction not found.');
        },
        error: function() { showError('Transaction not found.'); }
    });
}

// ── Render ────────────────────────────────────────────────────────────────────

function renderTransaction(tx) {
    document.title = truncateHash(tx.txid, 16) + ' — Inzyght';

    const isCoinbase = tx.vin && tx.vin.length > 0 && tx.vin[0].coinbase !== undefined && tx.vin[0].coinbase !== '';
    const saplingSpends  = tx.sapling_spends  || 0;
    const saplingOutputs = tx.sapling_outputs || 0;
    const joinsplits     = tx.vjoinsplit      || [];
    const isMempool      = !tx.blockhash;

    // TXID
    $('#hdr-txid').text(tx.txid);

    // Block link
    if (tx.blockhash) {
        $('#inf-block').attr('href', '/block/' + tx.blockhash).text(tx.blockhash);
    } else {
        $('#block-row').addClass('d-none');
        $('#mempool-row').removeClass('d-none');
    }
    if (!isMempool) $('#mempool-row').addClass('d-none');

    // Header fields
    if (tx.height) $('#inf-height').html(`<a href="/block/${tx.height}">${formatNumber(tx.height)}</a>`);
    if (tx.blocktime) $('#inf-time').text(formatTimestamp(tx.blocktime));
    $('#inf-confirmations').text(tx.confirmations ? formatNumber(tx.confirmations) : '0 (mempool)');
    if (tx.size) $('#inf-size').text(formatNumber(tx.size) + ' B');
    $('#inf-version').text(tx.version);
    if (tx.expiry_height) $('#inf-expiry').text(formatNumber(tx.expiry_height));

    // Fee
    if (!isCoinbase) {
        const sumVin  = (tx.vin  || []).reduce((s, i) => s + (getValue(i, 'value') || 0), 0);
        const sumVout = (tx.vout || []).reduce((s, o) => s + (getValue(o, 'value') || 0), 0);
        const sapling = tx.value_balance || 0;
        const sprout  = joinsplits.reduce((s, j) => s + (getValue(j, 'vpub_new') || 0) - (getValue(j, 'vpub_old') || 0), 0);
        const fee = sumVin - sumVout + sapling + sprout;
        $('#inf-fee').text(formatYec(fee));
    } else {
        $('#inf-fee').text('—');
    }

    // Type badges
    const badges = [];
    if (isCoinbase) badges.push('<span class="badge badge-coinbase me-1">Coinbase</span>');
    if (saplingSpends > 0 || saplingOutputs > 0)
        badges.push(`<span class="badge badge-sapling me-1">Sapling (${saplingSpends} in / ${saplingOutputs} out)</span>`);
    if (joinsplits.length > 0)
        badges.push(`<span class="badge badge-sprout me-1">Sprout (${joinsplits.length} joinsplits)</span>`);
    $('#type-badges').html(badges.join(''));

    // Inputs / Outputs headings with counts
    // vpub_new > 0: T→Sprout shielding (transparent input side)
    // vpub_old > 0: Sprout→T unshielding (transparent output side)
    const inCount  = isCoinbase ? 1 : (tx.vin || []).length + saplingSpends + joinsplits.filter(j => (getValue(j, 'vpub_new') || 0) > 0).length;
    const outCount = (tx.vout || []).length + saplingOutputs + joinsplits.filter(j => (getValue(j, 'vpub_old') || 0) > 0 || (getValue(j, 'vpub_new') || 0) > 0).length;
    $('#vin-heading').text(`Inputs (${inCount})`);
    $('#vout-heading').text(`Outputs (${outCount})`);

    const confirmations = tx.confirmations || 0;
    const isImmature = isCoinbase && confirmations < 100;
    const transparentInCount  = isCoinbase ? 0 : (tx.vin  || []).length;
    const transparentOutCount = (tx.vout || []).length;
    renderInputs(tx.vin || [], isCoinbase, saplingSpends, joinsplits, transparentOutCount);
    renderOutputs(tx.vout || [], saplingOutputs, tx.value_balance, joinsplits, isImmature, confirmations, transparentInCount, saplingSpends);

    initRawJsonModal(tx.txid);

    $('#loading').addClass('d-none');
    $('#tx-content').removeClass('d-none');
}

// glz::generic objects come as plain JS objects — helper to safely read a field
function getValue(obj, key) {
    if (obj === null || obj === undefined) return undefined;
    if (typeof obj === 'object' && !Array.isArray(obj)) return obj[key];
    return undefined;
}

function getAddresses(vout) {
    const spk = getValue(vout, 'scriptPubKey');
    if (!spk) return [];
    const addrs = getValue(spk, 'addresses');
    return Array.isArray(addrs) ? addrs : [];
}

function getVoutType(vout) {
    const spk = getValue(vout, 'scriptPubKey');
    return getValue(spk, 'type') || 'nonstandard';
}

function renderInputs(vin, isCoinbase, saplingSpends, joinsplits, transparentOutCount) {
    const container = $('#vin-list');

    if (isCoinbase) {
        container.append(`<div class="card border-0 bg-light mb-2 p-2 io-row text-muted fst-italic">Coinbase (newly generated coins)</div>`);
    } else {
        vin.forEach(inp => {
            const addr = getValue(inp, 'address');
            const value = getValue(inp, 'value');
            const txid  = getValue(inp, 'txid');
            const vout  = getValue(inp, 'vout');
            const addrHtml = addr
                ? `<a href="/address/${addr}" class="font-monospace">${addr}</a>`
                : `<span class="text-muted font-monospace">${truncateHash(txid)}:${vout}</span>`;
            container.append(`
                <div class="card border-0 bg-light mb-2 p-2 io-row d-flex justify-content-between align-items-center">
                    <span>${addrHtml}</span>
                    <span class="text-danger text-nowrap ms-3">${value != null ? '−' + formatYec(value) : ''}</span>
                </div>`);
        });

        if (saplingSpends > 0) {
            const label = transparentOutCount > 0
                ? `Sapling → transparent (unshielding): ${saplingSpends} spend(s)`
                : `Sapling private input: ${saplingSpends} spend(s)`;
            container.append(`<div class="card border-0 bg-light mb-2 p-2 io-row text-primary">${label}</div>`);
        }
    }
}

function renderOutputs(vout, saplingOutputs, valueBalance, joinsplits, isImmature, confirmations, transparentInCount, saplingSpends) {
    const container = $('#vout-list');

    if (isImmature) {
        const remaining = 100 - confirmations;
        container.append(`<div class="alert alert-warning py-1 px-2 mb-2 small">
            <i class="fa fa-lock"></i> Coinbase outputs are <strong>immature</strong> — spendable after 100 confirmations (${confirmations}/100, ${remaining} remaining).
        </div>`);
    }

    vout.forEach(out => {
        const addrs = getAddresses(out);
        const value = getValue(out, 'value');
        const spentTxid = getValue(out, 'spentTxId') || getValue(out, 'spent_txid') || '';
        const addrHtml = addrs.length > 0
            ? addrs.map(a => `<a href="/address/${a}" class="font-monospace">${a}</a>`).join(', ')
            : `<span class="text-muted">${getVoutType(out)}</span>`;
        const spentIndicator = isImmature
            ? `<span class="badge bg-warning text-dark ms-2" title="Not yet spendable">Immature</span>`
            : spentTxid
                ? `<a href="/transaction/${spentTxid}" class="text-danger fw-bold ms-2 text-decoration-none" title="Spent in ${spentTxid}">S</a>`
                : `<span class="text-success fw-bold ms-2" title="Unspent">U</span>`;
        container.append(`
            <div class="card border-0 bg-light mb-2 p-2 io-row d-flex justify-content-between align-items-center">
                <span>${addrHtml}</span>
                <span class="d-flex align-items-center text-nowrap ms-3">
                    <span class="text-success">${value != null ? '+' + formatYec(value) : ''}</span>${spentIndicator}
                </span>
            </div>`);
    });

    if (saplingOutputs > 0) {
        const net = valueBalance || 0;
        // Determine if this is shielding (T→Z), unshielding handled on input side,
        // or a pure private Z→Z Sapling transfer (no transparent counterpart)
        const isShielding = transparentInCount > 0;
        const isPurePrivate = !isShielding && (saplingSpends || 0) > 0;
        let txt;
        if (isShielding)
            txt = `Transparent → Sapling (shielding): ${saplingOutputs} output(s)`;
        else if (isPurePrivate)
            txt = `Sapling private output: ${saplingOutputs} output(s)`;
        else
            txt = `Sapling output: ${saplingOutputs} output(s)`;
        if (net !== 0) txt += ` (net: ${formatYec(Math.abs(net))})`;
        container.append(`<div class="card border-0 bg-light mb-2 p-2 io-row text-primary">${txt}</div>`);
    }

    joinsplits.forEach(js => {
        // vpub_old > 0: value exits joinsplit TO transparent pool (Sprout→T unshielding)
        // vpub_new > 0: value enters joinsplit FROM transparent pool (T→Sprout shielding)
        // Both zero: pure Sprout→Sprout (private, nothing shown here)
        const vpub_old = getValue(js, 'vpub_old') || 0;
        if (vpub_old > 0)
            container.append(`<div class="card border-0 bg-light mb-2 p-2 io-row" style="color:#6f42c1">Transparent → Sprout (shielding): ${formatYec(vpub_old)}</div>`);
        const vpub_new = getValue(js, 'vpub_new') || 0;
        if (vpub_new > 0)
            container.append(`<div class="card border-0 bg-light mb-2 p-2 io-row" style="color:#6f42c1">Sprout → transparent (unshielding): ${formatYec(vpub_new)}</div>`);
    });
}

// ── Copy TXID ─────────────────────────────────────────────────────────────────

function copyTxid() {
    navigator.clipboard.writeText($('#hdr-txid').text()).then(() => {
        const btn = $('button[onclick="copyTxid()"]');
        btn.html('<i class="fa fa-check"></i>');
        setTimeout(() => btn.html('<i class="fa fa-copy"></i>'), 2000);
    });
}

// ── Raw JSON modal ────────────────────────────────────────────────────────────

let rawJsonLoaded = false;

function initRawJsonModal(txid) {
    const modal = new bootstrap.Modal(document.getElementById('rawJsonModal'));

    $('#raw-json-btn').on('click', function() {
        modal.show();
        if (rawJsonLoaded) return;

        $.ajax({
            url: API_BASE + '/transactions/raw/' + txid,
            method: 'GET', dataType: 'text', timeout: 15000,
            success: function(text) {
                const codeEl = document.getElementById('raw-json-code');
                codeEl.textContent = JSON.stringify(JSON.parse(text), null, 2);
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
        navigator.clipboard.writeText(document.getElementById('raw-json-code').textContent).then(() => {
            $('#raw-copy-btn').html('<i class="fa fa-check"></i> Copied!');
            setTimeout(() => $('#raw-copy-btn').html('<i class="fa fa-copy"></i> Copy'), 2000);
        });
    });
}

// ── Init ──────────────────────────────────────────────────────────────────────

$(document).ready(function() {
    const txid = getTxid();
    if (!txid) {
        showError('No transaction ID in URL.');
        return;
    }
    fetchTransaction(txid);
});
