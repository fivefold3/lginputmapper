// Spatial focus navigation for the remote's D-pad. Any element with a
// data-fid attribute is focusable; arrows move to the nearest element in that
// direction, OK clicks, Back is reported to the app.
var KEY = { LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, ENTER: 13, BACK: 461, ESC: 27 };

function rect(el) { return el.getBoundingClientRect(); }

function focusables() {
  var scope = document.querySelector('[data-scope]') || document;
  return Array.prototype.filter.call(scope.querySelectorAll('[data-fid]'), function (el) {
    if (el.disabled || el.getAttribute('aria-hidden') === 'true') return false;
    var r = rect(el);
    return r.width > 0 && r.height > 0;
  });
}

function reveal(el) {
  if (el.scrollIntoViewIfNeeded) el.scrollIntoViewIfNeeded(false);
  else if (el.scrollIntoView) el.scrollIntoView(false);
}

function focus(el) {
  if (!el) return false;
  el.focus();
  reveal(el);
  return document.activeElement === el;
}

function focusFirst() {
  var list = focusables();
  var auto = list.filter(function (el) { return el.hasAttribute('data-autofocus'); })[0];
  return focus(auto || list[0]);
}

function move(dir) {
  var cur = document.activeElement;
  var list = focusables();
  if (!cur || !cur.hasAttribute('data-fid') || list.indexOf(cur) < 0) return focusFirst();
  var r = rect(cur);
  var best = null, bestScore = Infinity;
  list.forEach(function (el) {
    if (el === cur) return;
    var b = rect(el);
    var primary, overlap, secondary;
    if (dir === 'up') { primary = r.top - b.bottom; overlap = Math.min(r.right, b.right) - Math.max(r.left, b.left); secondary = Math.abs((b.left + b.right) / 2 - (r.left + r.right) / 2); }
    else if (dir === 'down') { primary = b.top - r.bottom; overlap = Math.min(r.right, b.right) - Math.max(r.left, b.left); secondary = Math.abs((b.left + b.right) / 2 - (r.left + r.right) / 2); }
    else if (dir === 'left') { primary = r.left - b.right; overlap = Math.min(r.bottom, b.bottom) - Math.max(r.top, b.top); secondary = Math.abs((b.top + b.bottom) / 2 - (r.top + r.bottom) / 2); }
    else { primary = b.left - r.right; overlap = Math.min(r.bottom, b.bottom) - Math.max(r.top, b.top); secondary = Math.abs((b.top + b.bottom) / 2 - (r.top + r.bottom) / 2); }
    if (primary < -8) return; // not in that direction
    var score = Math.max(primary, 0) + (overlap > 0 ? 0 : secondary * 2 + 400);
    if (score < bestScore) { bestScore = score; best = el; }
  });
  return best ? focus(best) : false;
}

function isTextInput(el) {
  return el && (el.tagName === 'TEXTAREA' || (el.tagName === 'INPUT' && /^(text|search|url|number|password)$/.test(el.type || 'text')));
}

function install(handlers) {
  document.addEventListener('keydown', function (ev) {
    var code = ev.keyCode;
    var active = document.activeElement;
    if (handlers.trace) handlers.trace('key ' + code + ' on ' + (active ? (active.getAttribute('data-fid') || active.tagName) : 'none'));
    if (code === KEY.BACK || code === KEY.ESC) {
      ev.preventDefault();
      if (isTextInput(active)) { active.blur(); focusFirst(); return; }
      handlers.back();
      return;
    }
    if (code === KEY.ENTER) {
      if (isTextInput(active)) { ev.preventDefault(); active.blur(); if (handlers.submit) handlers.submit(active); return; }
      // Activate the focused element ourselves: the remote's OK does not always
      // produce the keypress Chrome needs for native button activation.
      ev.preventDefault();
      if (active && active.hasAttribute('data-fid')) active.click();
      else focusFirst();
      return;
    }
    var dir = code === KEY.UP ? 'up' : code === KEY.DOWN ? 'down' : code === KEY.LEFT ? 'left' : code === KEY.RIGHT ? 'right' : null;
    if (!dir) return;
    if (isTextInput(active) && (dir === 'left' || dir === 'right')) return; // caret movement
    ev.preventDefault();
    move(dir);
  });
  // The Magic Remote pointer behaves like a mouse: keep focus in sync with it.
  document.addEventListener('mousedown', function (ev) {
    var el = ev.target;
    while (el && el !== document.body && !el.hasAttribute('data-fid')) el = el.parentNode;
    if (!el || el === document.body) ev.preventDefault(); // keep focus where it is
  });
  document.addEventListener('mouseover', function (ev) {
    var el = ev.target;
    while (el && el !== document.body && !el.hasAttribute('data-fid')) el = el.parentNode;
    if (el && el !== document.body && el !== document.activeElement) el.focus();
  });
}

module.exports = { install: install, focusFirst: focusFirst, focus: focus, focusables: focusables };
