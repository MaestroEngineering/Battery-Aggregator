// Applies the saved light/dark choice before first paint (no inline scripts: CSP).
(function () {
  try {
    var t = localStorage.getItem('bms-theme');
    if (t === 'light' || t === 'dark') document.documentElement.setAttribute('data-theme', t);
  } catch (e) { /* storage unavailable: follow the system setting */ }
})();

function bmsToggleTheme() {
  var root = document.documentElement;
  var cur = root.getAttribute('data-theme');
  if (!cur) cur = window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  var next = cur === 'dark' ? 'light' : 'dark';
  root.setAttribute('data-theme', next);
  try { localStorage.setItem('bms-theme', next); } catch (e) { /* ignore */ }
}

document.addEventListener('DOMContentLoaded', function () {
  var b = document.getElementById('theme');
  if (b) b.addEventListener('click', bmsToggleTheme);
});
