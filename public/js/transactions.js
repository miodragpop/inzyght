/**
 * Inzyght — All Transactions page
 * Uses DataTables server-side mode against /api/v1/transactions/all.
 */

const MONTHS = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];

function formatDate(ts) {
    if (!ts) return '—';
    const d   = new Date(ts * 1000);
    const day = String(d.getDate()).padStart(2, '0');
    const hh  = String(d.getHours()).padStart(2, '0');
    const mm  = String(d.getMinutes()).padStart(2, '0');
    const ss  = String(d.getSeconds()).padStart(2, '0');
    return `<span class="d-block">${day} ${MONTHS[d.getMonth()]} ${d.getFullYear()}</span>` +
           `<span class="d-block text-muted" style="font-size:0.78rem">${hh}:${mm}:${ss}</span>`;
}

$(document).ready(function () {
    $('#tx-table').DataTable({
        serverSide: true,
        processing: true,
        searching:  false,
        pageLength: 25,
        lengthMenu: [10, 25, 50, 100],
        ordering:   false,
        ajax: {
            url:  '/api/v1/transactions/all',
            type: 'GET',
        },
        columns: [
            {
                data: 'hash',
                orderable: false,
                render: (hash) =>
                    `<a href="/transaction/${hash}" class="text-decoration-none font-monospace hash-cell">${hash}</a>`
            },
            {
                data: 'height',
                render: (h) =>
                    `<a href="/block/${h}" class="text-decoration-none fw-semibold">${h.toLocaleString()}</a>`
            },
            {
                data: 'time',
                render: (ts) => `<small>${formatDate(ts)}</small>`
            },
            {
                data: 'is_coinbase',
                className: 'text-center',
                orderable: false,
                render: (cb) => cb
                    ? '<span class="badge bg-warning text-dark">Coinbase</span>'
                    : '<span class="badge bg-secondary">Regular</span>'
            }
        ],
        language: {
            processing:     '<i class="fa fa-spinner fa-spin"></i> Loading…',
            emptyTable:     'No transactions found.',
            loadingRecords: 'Loading…'
        }
    });
});
