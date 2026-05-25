/**
 * Inzyght — Dark / light mode toggle
 * Applies data-bs-theme on <html> and persists choice in localStorage.
 * Must be loaded (ideally in <head>) before page renders to avoid flash.
 */

(function () {
    const STORAGE_KEY = 'inzyght-theme';

    function getPreferred() {
        const stored = localStorage.getItem(STORAGE_KEY);
        if (stored) return stored;
        return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
    }

    function applyTheme(theme) {
        document.documentElement.setAttribute('data-bs-theme', theme);
        localStorage.setItem(STORAGE_KEY, theme);
        // Update toggle button icon/title once DOM is ready
        const btn = document.getElementById('theme-toggle');
        if (btn) {
            const icon = btn.querySelector('i');
            if (theme === 'dark') {
                icon.className = 'fa fa-sun-o';
                btn.title = 'Switch to light mode';
            } else {
                icon.className = 'fa fa-moon-o';
                btn.title = 'Switch to dark mode';
            }
        }
    }

    window.toggleTheme = function () {
        const current = document.documentElement.getAttribute('data-bs-theme') || 'light';
        applyTheme(current === 'dark' ? 'light' : 'dark');
    };

    // Called by each page's header fetch callback to sync the button icon
    window.syncThemeButton = function () {
        applyTheme(document.documentElement.getAttribute('data-bs-theme') || getPreferred());
    };

    // Apply immediately to avoid flash of wrong theme
    applyTheme(getPreferred());
})();
