/**
 * Global node-freshness indicator (navbar badge, all pages).
 *
 * Polls /api/v1/network/info — served from the backend's in-memory snapshot,
 * so polling costs no ycashd RPC — and shows the #node-stale-badge from the
 * shared header when the backend reports its snapshot as stale, i.e. ycashd
 * stopped answering RPC and the data on screen is frozen.
 */
(function () {
    const POLL_MS = 30000;       // normal cadence
    const POLL_STALE_MS = 5000;  // while stale: poll faster so recovery hides the badge promptly

    let timer = null;

    function schedule(ms) {
        clearTimeout(timer);
        timer = setTimeout(check, ms);
    }

    function formatAge(secs) {
        if (secs === undefined || secs === null || secs < 0) return 'unknown age';
        if (secs < 60) return secs + 's';
        if (secs < 3600) return Math.floor(secs / 60) + 'm';
        return Math.floor(secs / 3600) + 'h ' + Math.floor((secs % 3600) / 60) + 'm';
    }

    function setBadge(visible, title) {
        const badge = document.getElementById('node-stale-badge');
        if (!badge) return; // header partial not injected yet; next poll catches it
        badge.classList.toggle('d-none', !visible);
        if (title) badge.title = title;
    }

    // Fills the #version-info line (index page hero) with explorer and node
    // versions. No-op on pages without the element.
    function setVersionLine(d) {
        const el = document.getElementById('version-info');
        if (!el) return;
        const parts = [];
        if (d.explorer_version) parts.push('Inzyght v' + d.explorer_version);
        if (d.subversion) {
            // ycashd reports subversion as "/MagicBean:3.7.0/"
            const m = String(d.subversion).match(/^\/?([^:/]+):([^/]+)\/?$/);
            parts.push('ycashd ' + (m ? 'v' + m[2] : d.subversion));
        }
        el.textContent = parts.join(' · ');
    }

    function check() {
        fetch('/api/v1/network/info')
            .then(function (r) { return r.json(); })
            .then(function (resp) {
                const d = (resp && resp.data) || {};
                setVersionLine(d);
                if (d.stale) {
                    setBadge(true, 'ycashd is not answering — data shown is ' +
                                   formatAge(d.as_of_age_seconds) + ' old');
                    schedule(POLL_STALE_MS);
                } else {
                    setBadge(false);
                    schedule(POLL_MS);
                }
            })
            .catch(function () {
                setBadge(true, 'explorer backend unreachable');
                schedule(POLL_STALE_MS);
            });
    }

    document.addEventListener('DOMContentLoaded', function () {
        // Small delay so the async-injected header partial is in the DOM
        // before the first toggle attempt.
        setTimeout(check, 1500);
    });
})();
