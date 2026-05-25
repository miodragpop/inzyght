const API_PEERS = '/api/v1/network/peers';

// Convert ISO 3166-1 alpha-2 country code to flag emoji
function countryCodeToFlag(code) {
    if (!code || code.length !== 2) return '';
    const offset = 127397; // Unicode regional indicator offset
    return String.fromCodePoint(code.charCodeAt(0) + offset, code.charCodeAt(1) + offset);
}

function formatPing(ms) {
    if (!ms && ms !== 0) return '---';
    return ms.toFixed(1) + ' ms';
}

let map = null;
let peersTable = null;
let peersTableInitialized = false;

function initMap() {
    map = new jsVectorMap({
        selector: '#peer-map',
        map: 'world',
        backgroundColor: '#1a1a2e',
        regionStyle: {
            initial: { fill: '#2d3561', stroke: '#1a1a2e', strokeWidth: 0.5 },
            hover:   { fill: '#4a5280' }
        },
        markerStyle: {
            initial: { fill: '#00d4ff', stroke: '#fff', strokeWidth: 1, r: 5 },
            hover:   { fill: '#ff6b35', stroke: '#fff' }
        },
        markers: [],
        labels: { markers: { render: () => '' } }
    });
}

function updateMap(peers) {
    if (!map) return;

    const markers = peers
        .filter(p => p.lat !== 0 || p.lon !== 0)
        .map(p => ({
            name: p.ip + (p.city ? ` — ${p.city}` : ''),
            coords: [p.lat, p.lon]
        }));

    map.addMarkers(markers);
}

function initTable() {
    peersTable = $('#peers-table').DataTable({
        pageLength: 25,
        order: [[4, 'asc']], // default sort by ping
        columnDefs: [
            { orderable: false, targets: [3] } // direction badge not sortable
        ],
        language: {
            emptyTable: 'No peers connected',
            zeroRecords: 'No peers match the filter'
        }
    });
}

function renderTable(peers) {
    if (!peersTable) return;

    const rows = peers.map(p => [
        `<code>${p.ip}</code>`,
        `<span class="flag">${countryCodeToFlag(p.country_code)}</span>${p.country || '<span class="text-muted">Unknown</span>'}`,
        p.city || '<span class="text-muted">---</span>',
        p.inbound
            ? '<span class="badge badge-inbound">Inbound</span>'
            : '<span class="badge badge-outbound text-white">Outbound</span>',
        p.ping_ms != null ? p.ping_ms : 9999, // numeric for sorting, rendered below
        p.subver ? `<small class="text-muted">${p.subver}</small>` : '---',
        p.synced_blocks
    ]);

    const savedPage = peersTableInitialized ? peersTable.page() : 0;
    peersTable.clear().rows.add(rows).draw();
    peersTableInitialized = true;
    if (savedPage > 0) peersTable.page(savedPage).draw('page');

    // Replace raw ping number with formatted string after draw
    peersTable.rows().every(function(rowIdx) {
        const node = this.node();
        const raw  = peers[rowIdx] && peers[rowIdx].ping_ms;
        $('td:eq(4)', node).html(formatPing(raw));
    });
}

function updateAddnodeExport(peers) {
    const lines = peers.map(p => `addnode=${p.ip}`).join('\n');
    $('#addnode-text').val(lines);
}

function updateSummary(peers) {
    const inbound  = peers.filter(p => p.inbound).length;
    const outbound = peers.length - inbound;
    const countries = new Set(peers.map(p => p.country_code).filter(Boolean)).size;

    $('#total-peers').text(peers.length);
    $('#inbound-peers').text(inbound);
    $('#outbound-peers').text(outbound);
    $('#country-count').text(countries);
}

function fetchPeers() {
    $.ajax({
        url: API_PEERS,
        method: 'GET',
        dataType: 'json',
        timeout: 10000,
        success: function(response) {
            if (response.status === 'success' && response.data) {
                const peers = response.data;
                renderTable(peers);
                updateSummary(peers);
                updateMap(peers);
                updateAddnodeExport(peers);
            } else {
                $('#peers-tbody').html('<tr><td colspan="7" class="text-center text-muted">No data available</td></tr>');
            }
        },
        error: function() {
            $('#peers-tbody').html('<tr><td colspan="7" class="text-center text-danger">Failed to load peers</td></tr>');
        }
    });
}

$(document).ready(function() {
    initMap();
    initTable();
    fetchPeers();

    document.getElementById('peer-map').addEventListener('wheel', function(e) {
        if (!e.ctrlKey) e.stopImmediatePropagation();
    }, { capture: true, passive: true });
    setInterval(fetchPeers, 30000);

    $('#copy-addnode-btn').on('click', function() {
        const text = $('#addnode-text').val();
        navigator.clipboard.writeText(text).then(() => {
            const btn = $(this);
            btn.html('<i class="fa fa-check"></i> Copied!');
            setTimeout(() => btn.html('<i class="fa fa-copy"></i> Copy to Clipboard'), 2000);
        });
    });
});
