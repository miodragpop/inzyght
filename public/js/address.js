/**
 * Inzyght — Address detail page
 * Transaction history uses chunked fetching: backend scans 50k-block ranges
 * backwards from the chain tip, stopping once enough txids are found.
 * "Load more" requests the next chunk via the next_offset_height cursor.
 */

const API_BASE = '/api/v1';

// ── Helpers ───────────────────────────────────────────────────────────────────

function satToYec(sat) {
    return (Number(sat) / 1e8).toFixed(8) + ' YEC';
}

function formatNumber(n) {
    return Number(n).toLocaleString();
}

function showError(msg) {
    $('#loading').addClass('d-none');
    $('#error-msg').removeClass('d-none').text(msg);
}

function getAddress() {
    const parts = window.location.pathname.split('/');
    return parts[parts.length - 1];
}

// ── State ─────────────────────────────────────────────────────────────────────

let txTable          = null;
let nextOffsetHeight = -1;  // -1 = no more data
let loadingMore      = false;
let chainHeight      = 0;
let qrGenerated      = false;
const currentAddr    = getAddress();

// ── DataTables init ───────────────────────────────────────────────────────────

function initTable() {
    txTable = $('#tx-table').DataTable({
        data:       [],
        pageLength: 25,
        lengthMenu: [10, 25, 50, 100],
        searching:  false,
        order:      [],   // preserve server order (height desc)
        columns: [
            {
                data: 'txid',
                orderable: false,
                render: (txid) =>
                    `<a href="/transaction/${txid}" class="font-monospace text-decoration-none" style="font-size:0.8rem">${txid}</a>`
            },
            {
                data: 'height',
                render: (h) =>
                    `<a href="/block/${h}" class="text-decoration-none">${formatNumber(h)}</a>`
            },
            {
                data: 'time',
                render: (ts) => {
                    if (!ts) return '—';
                    const d  = new Date(ts * 1000);
                    const MONTHS = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
                    const day = String(d.getDate()).padStart(2, '0');
                    const hh  = String(d.getHours()).padStart(2, '0');
                    const mm  = String(d.getMinutes()).padStart(2, '0');
                    const ss  = String(d.getSeconds()).padStart(2, '0');
                    return `<small><span class="d-block">${day} ${MONTHS[d.getMonth()]} ${d.getFullYear()}</span>` +
                           `<span class="d-block text-muted" style="font-size:0.78rem">${hh}:${mm}:${ss}</span></small>`;
                }
            },
            {
                data: 'net_change',
                className: 'text-end',
                render: (v, type, row) => {
                    const sign  = v >= 0 ? '+' : '';
                    const color = v >= 0 ? '#198754' : '#dc3545';
                    const confirmations = chainHeight > 0 ? chainHeight - row.height + 1 : 0;
                    const immature = row.is_coinbase && confirmations < 100;
                    const badge = immature
                        ? ` <span class="badge bg-warning text-dark" title="${confirmations}/100 confirmations">Immature</span>`
                        : '';
                    return `<span style="color:${color};font-weight:600">${sign}${satToYec(v)}</span>${badge}`;
                }
            }
        ],
        language: { emptyTable: 'No transactions found.' }
    });
}

// ── Fetch helpers ─────────────────────────────────────────────────────────────

function buildUrl(addr, offsetHeight) {
    let url = `${API_BASE}/addresses/${addr}`;
    if (offsetHeight > 0) url += `?offset_height=${offsetHeight}`;
    return url;
}

function appendTransactions(transactions, hasMore, newNextOffset, newChainHeight) {
    if (newChainHeight) chainHeight = newChainHeight;
    if (transactions && transactions.length) {
        txTable.rows.add(transactions).draw(false);
    }
    nextOffsetHeight = hasMore ? newNextOffset : -1;
    if (nextOffsetHeight > 0) {
        $('#load-more-wrap').removeClass('d-none');
    } else {
        $('#load-more-wrap').addClass('d-none');
    }
}

// ── Initial fetch ─────────────────────────────────────────────────────────────

function fetchAddress(addr) {
    $.ajax({
        url: buildUrl(addr, 0),
        method: 'GET', dataType: 'json', timeout: 60000,
        success: function(r) {
            if (r.status === 'success') renderAddress(r.data);
            else showError(r.message || 'Address not found.');
        },
        error: function() { showError('Address not found or RPC error.'); }
    });
}

// ── Render on first load ──────────────────────────────────────────────────────

function renderAddress(data) {
    document.title = data.address.slice(0, 16) + '… — Inzyght';

    $('#hdr-address').text(data.address);
    $('#inf-balance').text(satToYec(data.balance));
    $('#inf-received').text(satToYec(data.received));
    $('#inf-sent').text(satToYec(data.sent));

    $('#inf-txcount').text(formatNumber(data.tx_count));
    renderMempool(data.mempool || []);
    appendTransactions(data.transactions, data.has_more, data.next_offset_height, data.chain_height);

    initQrModal(data.address);

    $('#loading').addClass('d-none');
    $('#addr-content').removeClass('d-none');
}

// ── Load more (subsequent chunks) ────────────────────────────────────────────

function loadMore() {
    if (loadingMore || nextOffsetHeight <= 0) return;
    loadingMore = true;

    const btn = $('#load-more-btn');
    btn.prop('disabled', true).html('<i class="fa fa-spinner fa-spin"></i> Loading…');

    $.ajax({
        url: buildUrl(currentAddr, nextOffsetHeight),
        method: 'GET', dataType: 'json', timeout: 60000,
        success: function(r) {
            if (r.status === 'success') {
                appendTransactions(r.data.transactions, r.data.has_more, r.data.next_offset_height, r.data.chain_height);
            }
        },
        error: function() { /* leave button visible so user can retry */ },
        complete: function() {
            loadingMore = false;
            btn.prop('disabled', false).html('Load older transactions');
        }
    });
}

// ── Mempool ───────────────────────────────────────────────────────────────────

function renderMempool(mempool) {
    if (!mempool.length) return;
    $('#mempool-section').removeClass('d-none');
    const list = $('#mempool-list');
    mempool.forEach(m => {
        const netClass = m.net_change >= 0 ? 'net-positive' : 'net-negative';
        const sign     = m.net_change >= 0 ? '+' : '';
        list.append(`
            <div class="card border-0 bg-warning bg-opacity-10 border-warning mb-2 p-2 d-flex flex-row justify-content-between align-items-center" style="border-left:3px solid #ffc107!important">
                <a href="/transaction/${m.txid}" class="font-monospace text-decoration-none small">${m.txid}</a>
                <span class="${netClass} ms-3 text-nowrap">${sign}${satToYec(m.net_change)}</span>
            </div>`);
    });
}

// ── Copy address ──────────────────────────────────────────────────────────────

function copyAddress() {
    navigator.clipboard.writeText($('#hdr-address').text()).then(() => {
        const btn = $('button[onclick="copyAddress()"]');
        btn.html('<i class="fa fa-check"></i>');
        setTimeout(() => btn.html('<i class="fa fa-copy"></i>'), 2000);
    });
}

// ── QR code ───────────────────────────────────────────────────────────────────

function initQrModal(address) {
    document.getElementById('qrModal').addEventListener('show.bs.modal', function() {
        if (qrGenerated) return;
        $('#qr-address').text(address);
        new QRCode(document.getElementById('qr-canvas'), {
            text: address,
            width: 220,
            height: 220,
            colorDark: '#000000',
            colorLight: '#ffffff',
            correctLevel: QRCode.CorrectLevel.M
        });
        qrGenerated = true;
    });
}

// ── Init ──────────────────────────────────────────────────────────────────────

$(document).ready(function() {
    if (!currentAddr) { showError('No address in URL.'); return; }
    initTable();
    fetchAddress(currentAddr);
});
