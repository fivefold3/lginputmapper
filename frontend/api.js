// Luna transport to our service plus the Homebrew Channel service.
var SERVICE = 'luna://com.lginputmapper.app.service';
var HBCHANNEL = 'luna://org.webosbrew.hbchannel.service';

var TIMEOUT_MS = 10000;

function request(uri, method, parameters) {
  return new Promise(function (resolve, reject) {
    if (!window.webOS || !webOS.service) {
      reject(new Error('webOS.service is not available (not running on a TV?)'));
      return;
    }
    var done = false;
    var timer = setTimeout(function () {
      if (done) return;
      done = true;
      var e = new Error('No answer from ' + uri.replace('luna://', '') + '/' + method + ' after ' + (TIMEOUT_MS / 1000) + 's');
      e.code = 'timeout';
      reject(e);
    }, TIMEOUT_MS);
    webOS.service.request(uri, {
      method: method,
      parameters: parameters || {},
      onSuccess: function (res) { if (done) return; done = true; clearTimeout(timer); resolve(res); },
      onFailure: function (err) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        var e = new Error((err && err.errorText) || ('luna error ' + (err && err.errorCode)));
        e.code = err && (err.errorCode || err.code);
        reject(e);
      },
    });
  });
}

module.exports = {
  call: function (method, params) { return request(SERVICE, method, params); },
  hbchannel: function (method, params) { return request(HBCHANNEL, method, params); },
};
