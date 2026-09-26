// Mappings whose "acts as" chain leads back to their own button, e.g. Netflix
// acts as LG Channels and LG Channels acts as Netflix. The daemon makes those
// buttons do nothing until the loop is broken; the app shows them in red. Same rules
// as mark_loops() in the daemon: enabled mappings only, press and hold actions.
function replaceTargets(m) {
  var out = [];
  if (!m || m.enabled === false) return out;
  if (m.action && m.action.type === 'replace') out.push(m.action.to);
  if (m.hold && m.hold.type === 'replace') out.push(m.hold.to);
  return out;
}

// Returns { <key code>: [key codes in its loop, starting with itself] } for
// every mapping that is part of a loop.
function findLoops(mappings) {
  var byKey = {};
  (mappings || []).forEach(function (m) { byKey[m.key] = m; });
  var loops = {};
  (mappings || []).forEach(function (start) {
    // depth-first search for a path back to start, remembering the path
    var seen = {};
    var stack = replaceTargets(start).map(function (k) { return [k, [start.key]]; });
    while (stack.length) {
      var top = stack.pop();
      var k = top[0], path = top[1];
      if (k === start.key) { loops[start.key] = path; return; }
      if (seen[k]) continue;
      seen[k] = true;
      replaceTargets(byKey[k]).forEach(function (t) { stack.push([t, path.concat(k)]); });
    }
  });
  return loops;
}

module.exports = { findLoops: findLoops };
