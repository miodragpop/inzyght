// Supply-by-pool chart.
//
// The backend (/api/v1/supply/series) returns cumulative supply per value pool
// over a height range, downsampled to a target point count. We render it with
// uPlot using BLOCK HEIGHT as the x-axis (height is the primary axis here; the
// date for a point is shown in the hover tooltip). Changing the visible x range
// — by pan, wheel zoom, or box zoom — re-fetches that height window at higher
// resolution, so zoomed-in views approach full per-block detail while the
// zoomed-out view stays light.
//
// Gestures:  drag = pan,  Shift+drag = box zoom,  Ctrl+wheel = zoom at cursor.

(function () {
    'use strict';

    const POOLS = [
        { key: 'total',       label: 'Total',       color: '#0d6efd', width: 2 },
        { key: 'transparent', label: 'Transparent', color: '#fd7e14', width: 1.5 },
        { key: 'sprout',      label: 'Sprout',      color: '#6f42c1', width: 1.5 },
        { key: 'sapling',     label: 'Sapling',     color: '#198754', width: 1.5 },
    ];

    const el = {
        chart:    document.getElementById('supply-chart'),
        loading:  document.getElementById('chart-loading'),
        sync:     document.getElementById('sync-banner'),
        error:    document.getElementById('error-banner'),
        reset:    document.getElementById('reset-zoom'),
        range:    document.getElementById('range-label'),
    };

    let chart = null;
    let chainMin = null, chainMax = null;   // full chain height bounds
    // The currently displayed points' (height → time) pairs, used to label the
    // hovered block's date in the tooltip.
    let curHeights = [], curTimes = [];
    let curFrom = null, curTo = null;       // height window currently displayed
    let fetchToken = 0;                      // guards against out-of-order responses
    let yZoom = null;                        // {min,max} when y is manually zoomed, else null
    let curBucket = 1;                       // sampling stride of the loaded data
    let pendingFrom = null, pendingTo = null; // height window of the in-flight fetch

    function fmtYEC(v) {
        if (v == null) return '–';
        return v.toLocaleString(undefined, { maximumFractionDigits: 2 }) + ' YEC';
    }

    function fmtHeight(v) {
        return Math.round(v).toLocaleString();
    }

    // Date for a given block height — found by binary-searching the displayed
    // (sorted) heights. Used in the tooltip header so the date is on demand.
    function dateAtHeight(height) {
        if (!curHeights.length) return '';
        let lo = 0, hi = curHeights.length - 1;
        if (height <= curHeights[0]) lo = 0;
        else if (height >= curHeights[hi]) lo = hi;
        else {
            while (lo < hi) {
                const mid = (lo + hi) >> 1;
                if (curHeights[mid] < height) lo = mid + 1; else hi = mid;
            }
        }
        const secs = curTimes[lo];
        if (secs == null) return '';
        return new Date(secs * 1000).toLocaleString();
    }

    // Reset is available whenever the view is zoomed on either axis: x is a
    // sub-range of the chain, or y has been manually pinned.
    function updateResetState() {
        const xZoomed = !(curFrom <= chainMin && curTo >= chainMax);
        el.reset.disabled = !(xZoomed || yZoom);
    }

    function showError(msg) {
        el.error.textContent = msg;
        el.error.classList.remove('d-none');
    }

    function themeStroke() {
        const dark = document.documentElement.getAttribute('data-bs-theme') === 'dark';
        return {
            axis: dark ? '#adb5bd' : '#495057',
            grid: dark ? 'rgba(255,255,255,0.08)' : 'rgba(0,0,0,0.07)',
        };
    }

    function buildChart(data) {
        const t = themeStroke();
        const series = [
            {
                // x = block height. The legend/tooltip header shows the block
                // plus its date.
                label: 'Block',
                value: (self, v) => v == null ? '–' : `#${fmtHeight(v)} · ${dateAtHeight(v)}`,
            },
            ...POOLS.map(p => ({
                label: p.label,
                stroke: p.color,
                width: p.width,
                value: (self, v) => fmtYEC(v),
                points: { show: false },
            })),
        ];

        const opts = {
            width: el.chart.clientWidth || 900,
            height: 460,
            series,
            scales: { x: { time: false } },   // x is block height, not a timestamp
            axes: [
                {
                    stroke: t.axis,
                    grid: { stroke: t.grid },
                    values: (self, ticks) => ticks.map(v =>
                        v >= 1e6 ? (v / 1e6).toFixed(2) + 'M'
                      : v >= 1e3 ? (v / 1e3).toFixed(0) + 'k'
                      : fmtHeight(v)),
                },
                {
                    stroke: t.axis,
                    grid: { stroke: t.grid },
                    size: 70,
                    values: (self, ticks) => ticks.map(v =>
                        v >= 1e6 ? (v / 1e6).toFixed(1) + 'M'
                      : v >= 1e3 ? (v / 1e3).toFixed(0) + 'k'
                      : v),
                },
            ],
            // uPlot's built-in drag-select can't be gated by a modifier key, and
            // it would clash with our plain-drag pan. So we disable it and drive
            // all three gestures ourselves in attachPanZoom: drag = pan,
            // Shift+drag = box zoom, Ctrl+wheel = zoom at cursor.
            cursor: { drag: { x: false, y: false, setScale: false } },
            legend: { live: true },
            hooks: {
                setScale: [
                    (self, key) => {
                        if (key === 'y') {
                            const ys = self.scales.y;
                            yZoom = ys.auto ? null : { min: ys.min, max: ys.max };
                            updateResetState();
                            return;
                        }
                        if (key !== 'x') return;
                        const { min, max } = self.scales.x;
                        if (min == null || max == null) return;
                        // x is already block height — no time→height mapping needed.
                        maybeRefetch(min, max);
                    },
                ],
            },
        };

        chart = new uPlot(opts, data, el.chart);
        attachPanZoom(chart);
    }

    // ── Pan, box zoom, and wheel zoom ──────────────────────────────────────────
    // uPlot has no built-in pan or wheel zoom, and its drag-select can't be
    // modifier-gated, so we implement all three here by driving the scales
    // directly. Every x-scale change fires setScale('x') → maybeRefetch.
    function attachPanZoom(u) {
        const over = u.over;   // the plot's interaction surface

        // A translucent rectangle we draw during a Shift+drag box zoom.
        const box = document.createElement('div');
        Object.assign(box.style, {
            position: 'absolute', pointerEvents: 'none', display: 'none',
            background: 'rgba(13,110,253,0.15)', border: '1px solid rgba(13,110,253,0.6)',
        });
        over.appendChild(box);

        let mode = null;                       // 'pan' | 'zoom' | null
        let startX = 0, startY = 0;            // pointer at gesture start (CSS px in `over`)
        let panMin = 0, panMax = 0;            // x-scale snapshot for panning

        function localPos(e) {
            const rect = over.getBoundingClientRect();
            return { x: e.clientX - rect.left, y: e.clientY - rect.top, rect };
        }

        over.addEventListener('mousedown', (e) => {
            if (e.button !== 0) return;
            const p = localPos(e);
            startX = p.x; startY = p.y;
            if (e.shiftKey) {
                mode = 'zoom';
                Object.assign(box.style, { display: 'block', left: startX + 'px',
                    top: startY + 'px', width: '0px', height: '0px' });
            } else {
                mode = 'pan';
                panMin = u.scales.x.min;
                panMax = u.scales.x.max;
                over.style.cursor = 'grabbing';
            }
            e.preventDefault();
        });

        window.addEventListener('mousemove', (e) => {
            if (!mode) return;
            const p = localPos(e);
            if (mode === 'pan') {
                const perPx = (panMax - panMin) / p.rect.width;
                const dx = (p.x - startX) * perPx;
                // Dragging right reveals earlier blocks.
                let newMin = panMin - dx, newMax = panMax - dx;
                const span = newMax - newMin;
                if (newMin < chainMin) { newMin = chainMin; newMax = chainMin + span; }
                if (newMax > chainMax) { newMax = chainMax; newMin = chainMax - span; }
                u.setScale('x', { min: newMin, max: newMax });
            } else {
                // Draw the zoom rectangle (clamped to the plot area).
                const x = Math.max(0, Math.min(p.x, p.rect.width));
                const y = Math.max(0, Math.min(p.y, p.rect.height));
                Object.assign(box.style, {
                    left: Math.min(startX, x) + 'px', top: Math.min(startY, y) + 'px',
                    width: Math.abs(x - startX) + 'px', height: Math.abs(y - startY) + 'px',
                });
            }
        });

        window.addEventListener('mouseup', (e) => {
            if (!mode) return;
            if (mode === 'pan') {
                over.style.cursor = 'grab';
            } else {
                box.style.display = 'none';
                const p = localPos(e);
                const x0 = Math.min(startX, p.x), x1 = Math.max(startX, p.x);
                const y0 = Math.min(startY, p.y), y1 = Math.max(startY, p.y);
                // Ignore an accidental tiny box.
                if (x1 - x0 > 4 && y1 - y0 > 4) {
                    const xa = u.posToVal(x0, 'x'), xb = u.posToVal(x1, 'x');
                    // y screen coords are top-down; posToVal handles the flip.
                    const ya = u.posToVal(y1, 'y'), yb = u.posToVal(y0, 'y');
                    u.batch(() => {
                        u.setScale('x', { min: Math.min(xa, xb), max: Math.max(xa, xb) });
                        u.setScale('y', { min: Math.min(ya, yb), max: Math.max(ya, yb) });
                    });
                }
            }
            mode = null;
        });

        // ── Ctrl+wheel to zoom x around the cursor ──
        over.addEventListener('wheel', (e) => {
            if (!e.ctrlKey) return;   // plain scroll still scrolls the page
            e.preventDefault();
            const { min, max } = u.scales.x;
            const cursorVal = u.posToVal(localPos(e).x, 'x');
            const factor = e.deltaY < 0 ? 0.8 : 1.25;   // up = zoom in
            let newMin = cursorVal - (cursorVal - min) * factor;
            let newMax = cursorVal + (max - cursorVal) * factor;
            newMin = Math.max(newMin, chainMin);
            newMax = Math.min(newMax, chainMax);
            if (newMax - newMin < 1) return;            // degenerate range guard
            u.setScale('x', { min: newMin, max: newMax });
        }, { passive: false });

        over.style.cursor = 'grab';
    }

    function applyPayload(payload) {
        curHeights = payload.h;
        curTimes   = payload.t;
        curFrom    = payload.from;
        curTo      = payload.to;
        curBucket  = payload.bucket;
        pendingFrom = pendingTo = null;   // the in-flight fetch has landed

        // x = height; series order matches POOLS with Total first as labelled.
        const data = [
            payload.h,
            payload.total,
            payload.transparent,
            payload.sprout,
            payload.sapling,
        ];

        if (!chart) {
            el.loading.classList.add('d-none');
            buildChart(data);
        } else {
            chart.setData(data);
            // setData re-auto-ranges y. If the user pinned a y-zoom, restore it
            // so re-fetching higher-res data doesn't snap the value axis back.
            if (yZoom) chart.setScale('y', { min: yZoom.min, max: yZoom.max });
        }

        updateResetState();

        const span = payload.to - payload.from + 1;
        el.range.textContent =
            `Showing blocks ${payload.from.toLocaleString()}–${payload.to.toLocaleString()} ` +
            `(${payload.t.length.toLocaleString()} points, 1 per ${payload.bucket} block${payload.bucket > 1 ? 's' : ''} of ${span.toLocaleString()}).`;
    }

    // Decide whether an x-scale change is a real zoom/pan worth re-fetching, or
    // just jitter. setData re-fires setScale('x') as it auto-ranges, and the
    // sampled data's first/last heights never land exactly on the requested
    // bounds (a sample sits at from+bucket-1, not from) — so a naive "fetch on
    // every setScale" loops forever with the start point drifting. We only
    // re-fetch when the requested window differs from what's already loaded (or
    // in flight) by more than the current sampling stride.
    let refetchTimer = null;
    function maybeRefetch(from, to) {
        from = Math.round(from);
        to   = Math.round(to);
        const refFrom = pendingFrom != null ? pendingFrom : curFrom;
        const refTo   = pendingTo   != null ? pendingTo   : curTo;
        const tol = Math.max(curBucket, 1);
        if (refFrom != null &&
            Math.abs(from - refFrom) <= tol &&
            Math.abs(to   - refTo)   <= tol) {
            return;   // within sampling resolution — not a genuine new range
        }
        // Pan/wheel fire setScale continuously; the chart redraws instantly from
        // the data already in memory, but we debounce the higher-res re-fetch so
        // a gesture issues one request when it settles rather than dozens.
        clearTimeout(refetchTimer);
        refetchTimer = setTimeout(() => loadRange(from, to), 180);
    }

    function loadRange(from, to) {
        pendingFrom = from != null ? Math.floor(from) : chainMin;
        pendingTo   = to   != null ? Math.ceil(to)   : chainMax;
        const token = ++fetchToken;
        const width = Math.max(el.chart.clientWidth || 900, 300);
        const params = new URLSearchParams();
        if (from != null) params.set('from', Math.floor(from));
        if (to != null)   params.set('to', Math.ceil(to));
        params.set('points', String(Math.min(4000, Math.round(width * 1.5))));

        fetch('/api/v1/supply/series?' + params.toString())
            .then(r => r.json())
            .then(payload => {
                if (token !== fetchToken) return;   // a newer request superseded this
                if (payload.status !== 'success') {
                    pendingFrom = pendingTo = null;
                    showError(payload.message || 'Failed to load supply data.');
                    return;
                }
                if (payload.syncing) {
                    pendingFrom = pendingTo = null;
                    el.loading.classList.add('d-none');
                    el.sync.classList.remove('d-none');
                    return;
                }
                if (chainMin == null) { chainMin = payload.chain_min; chainMax = payload.chain_max; }
                if (!payload.t.length) {
                    pendingFrom = pendingTo = null;
                    el.loading.classList.add('d-none');
                    showError('No supply data available yet.');
                    return;
                }
                applyPayload(payload);
            })
            .catch(err => {
                if (token !== fetchToken) return;
                pendingFrom = pendingTo = null;
                el.loading.classList.add('d-none');
                showError('Network error loading supply data.');
                console.error(err);
            });
    }

    el.reset.addEventListener('click', () => {
        el.reset.disabled = true;
        yZoom = null;
        if (chart) {
            chart.setScale('y', { min: null, max: null });  // null,null → auto
        }
        loadRange(chainMin, chainMax);
    });

    // Keep the chart sized to its container, and recolour axes on theme switch.
    let resizeTimer = null;
    window.addEventListener('resize', () => {
        if (!chart) return;
        clearTimeout(resizeTimer);
        resizeTimer = setTimeout(() => {
            chart.setSize({ width: el.chart.clientWidth || 900, height: 460 });
        }, 150);
    });

    new MutationObserver(() => {
        if (!chart) return;
        const t = themeStroke();
        chart.axes.forEach(a => { a.stroke = t.axis; if (a.grid) a.grid.stroke = t.grid; });
        chart.redraw();
    }).observe(document.documentElement, { attributes: true, attributeFilter: ['data-bs-theme'] });

    // Initial load: full chain.
    loadRange(null, null);
})();
