(function () {
  var mc = navigator.mediaCapabilities;
  if (!mc || typeof mc.decodingInfo !== 'function')
    return;
  var engine = mc.decodingInfo.bind(mc);
  function capability(config, ks) {
    return [{ contentType: config.contentType,
              robustness: (ks && ks.robustness) || '',
              encryptionScheme: ks ? ks.encryptionScheme : null }];
  }
  function result(config, access) {
    return { supported: !!access, smooth: !!access, powerEfficient: false,
             keySystemAccess: access, configuration: config };
  }
  function decodingInfo(config) {
    var ks = config && config.keySystemConfiguration;
    if (!ks || config.type !== 'media-source')
      return engine(config);
    if (typeof navigator.requestMediaKeySystemAccess !== 'function')
      return Promise.resolve(result(config, null));
    var request = {
      initDataTypes: ks.initDataType ? [ks.initDataType] : [],
      distinctiveIdentifier: ks.distinctiveIdentifier || 'optional',
      persistentState: ks.persistentState || 'optional'
    };
    if (ks.sessionTypes)
      request.sessionTypes = ks.sessionTypes;
    if (config.video)
      request.videoCapabilities = capability(config.video, ks.video);
    if (config.audio)
      request.audioCapabilities = capability(config.audio, ks.audio);
    return navigator.requestMediaKeySystemAccess(ks.keySystem, [request]).then(
      function (access) { return result(config, access); },
      function () { return result(config, null); });
  }
  /* players such as Shaka replace decodingInfo() with an isTypeSupported() check */
  Object.defineProperty(mc, 'decodingInfo', {
    get: function () { return decodingInfo; },
    set: function () {},
    enumerable: true
  });
})();
