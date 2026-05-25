/**
 * Inzyght — All Blocks page
 * Uses DataTables server-side mode against /api/v1/blocks (draw/start/length params).
 */

const MONTHS = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];

function formatBlockDate(ts) {
    if (!ts) return '—';
    const d = new Date(ts * 1000);
    const day   = String(d.getDate()).padStart(2, '0');
    const month = MONTHS[d.getMonth()];
    const year  = d.getFullYear();
    const hh    = String(d.getHours()).padStart(2, '0');
    const mm    = String(d.getMinutes()).padStart(2, '0');
    const ss    = String(d.getSeconds()).padStart(2, '0');
    return `<span class="d-block">${day} ${month} ${year}</span>` +
           `<span class="d-block text-muted" style="font-size:0.78rem">${hh}:${mm}:${ss}</span>`;
}

function formatNumber(n) {
    return new Intl.NumberFormat('fr-FR').format(n).replace(/\s/g, '\u202F');
}

$(document).ready(function () {
    $('#blocks-table').DataTable({
        serverSide:  true,
        processing:  true,
        searching:   false,          // server-side source — search belongs to the global search bar
        pageLength:  25,
        lengthMenu:  [10, 25, 50, 100],
        order:       [[0, 'desc']],  // height descending by default
        ordering:    false,          // server always returns height-desc; disable client re-sort
        ajax: {
            url:  '/api/v1/blocks',
            type: 'GET',
            // DataTables sends draw, start, length automatically
        },
        columns: [
            {
                data: 'height',
                render: (h) =>
                    `<a href="/block/${h}" class="text-decoration-none fw-semibold">${formatNumber(h)}</a>`
            },
            {
                data: 'hash',
                orderable: false,
                render: (hash) =>
                    `<a href="/block/${hash}" class="text-decoration-none font-monospace hash-cell">${hash}</a>`
            },
            {
                data: 'time',
                render: (ts) => `<small>${formatBlockDate(ts)}</small>`
            },
            {
                data: 'tx',
                className: 'text-center',
                render: (n) => `<span class="badge bg-info text-dark">${n}</span>`
            },
            {
                data: 'miner',
                orderable: false,
                render: (addr) => addr
                    ? `<small class="miner-cell"><a href="/address/${addr}" class="text-decoration-none font-monospace">${addr}</a></small>`
                    : '<small class="text-muted">—</small>'
            },
            {
                data: 'size',
                className: 'text-end',
                render: (s) => `<small>${formatNumber(s)} B</small>`
            },
            {
                data: 'reward',
                className: 'text-end',
                render: (r) => `<strong class="text-success">${parseFloat(r).toFixed(8)} YEC</strong>`
            }
        ],
        language: {
            processing:  '<i class="fa fa-spinner fa-spin"></i> Loading…',
            emptyTable:  'No blocks found.',
            loadingRecords: 'Loading…'
        }
    });
});
