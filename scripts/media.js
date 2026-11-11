(function () {
  var post = window.webkit.messageHandlers.kodimedia;
  var token = Math.random().toString(36).slice(2) + Date.now().toString(36);
  var media = new Set();
  var last = -1;
  function bits(m) {
    var s = 0;
    if (m.networkState === m.NETWORK_EMPTY || m.error)
      return 0;
    if (m instanceof HTMLVideoElement && m.videoWidth > 0)
      s |= 1;
    if ((m.audioTracks && m.audioTracks.length) || m.webkitAudioDecodedByteCount > 0)
      s |= 2;
    if (!m.paused && !m.ended)
      s |= 4;
    return s;
  }
  function update(e) {
    var s = 0;
    if (e.target instanceof HTMLMediaElement)
      media.add(e.target);
    media.forEach(function (m) { s |= bits(m); });
    if (s === last)
      return;
    last = s;
    post.postMessage(token + ':' + s);
  }
  ['play', 'playing', 'pause', 'ended', 'emptied', 'error', 'loadedmetadata',
   'loadeddata', 'timeupdate'].forEach(function (t) {
    document.addEventListener(t, update, true);
  });
  window.addEventListener('pagehide', function () {
    post.postMessage(token + ':gone');
  }, true);
})();
