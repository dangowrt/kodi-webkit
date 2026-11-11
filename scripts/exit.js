(function () {
  var kodi = window.webkit.messageHandlers.kodi;
  window.close = function () { kodi.postMessage('exit'); };
})();
