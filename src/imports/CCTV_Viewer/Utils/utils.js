// Helpers
function streamScheme(raw) {
    var match = String(raw || "").trim().match(/^(rtsp|http|https):\/\//i);
    return match ? match[1].toLowerCase() : "";
}

function isSupportedStreamUrl(raw) {
    return streamScheme(raw) !== "";
}

function safeStreamUrlDecode(text) {
    try {
        return decodeURIComponent(text);
    } catch (e) {
        return text;
    }
}

// Split user and password at the last @, then encode them so a password with @, # or % survives.
// The path and query are kept exactly as entered. Returns { url, host, scheme } or null; does not open the address.
function encodeStreamUrl(raw) {
    var s = String(raw || "").trim();
    var scheme = streamScheme(s);
    if (scheme === "")
        return null;
    var rest = s.substring(scheme.length + 3);
    var slash = rest.indexOf("/");
    var main = slash === -1 ? rest : rest.substring(0, slash);
    var path = slash === -1 ? "" : rest.substring(slash);
    var at = main.lastIndexOf("@");
    var hostport = main;
    var user = "";
    var pass = "";
    if (at !== -1) {
        var creds = main.substring(0, at);
        hostport = main.substring(at + 1);
        var colon = creds.indexOf(":");
        if (colon !== -1) {
            user = safeStreamUrlDecode(creds.substring(0, colon));
            pass = safeStreamUrlDecode(creds.substring(colon + 1));
        } else {
            user = safeStreamUrlDecode(creds);
        }
    }
    var host = "";
    if (hostport.charAt(0) === "[") {
        var closeBracket = hostport.indexOf("]");
        if (closeBracket > 1)
            host = hostport.substring(1, closeBracket);
    } else {
        host = hostport.split(":")[0];
    }
    if (host.length === 0)
        return null;
    var credsOut = "";
    if (user.length > 0 || pass.length > 0) {
        credsOut = encodeURIComponent(user);
        if (pass.length > 0)
            credsOut += ":" + encodeURIComponent(pass);
        credsOut += "@";
    }
    return { url: scheme + "://" + credsOut + hostport + path, host: host, scheme: scheme };
}

// The form shows the password decoded. Saving encodes it again and preserves the detected scheme.
function decodeStreamForEdit(stored) {
    var parsed = encodeStreamUrl(stored);
    if (!parsed)
        return String(stored || "");
    var s = String(stored).trim();
    var rest = s.substring(parsed.scheme.length + 3);
    var slash = rest.indexOf("/");
    var main = slash === -1 ? rest : rest.substring(0, slash);
    var path = slash === -1 ? "" : rest.substring(slash);
    var at = main.lastIndexOf("@");
    if (at === -1)
        return s;
    var creds = main.substring(0, at);
    var hostport = main.substring(at + 1);
    var colon = creds.indexOf(":");
    var user = colon === -1 ? safeStreamUrlDecode(creds) : safeStreamUrlDecode(creds.substring(0, colon));
    var pass = colon === -1 ? "" : safeStreamUrlDecode(creds.substring(colon + 1));
    var shown = parsed.scheme + "://";
    if (user.length > 0 || pass.length > 0) {
        shown += user;
        if (pass.length > 0)
            shown += ":" + pass;
        shown += "@";
    }
    return shown + hostport + path;
}

function currentModel() {
    var win = rootWindow.activeLayoutWindow ? rootWindow.activeLayoutWindow : rootWindow;
    return layoutsCollectionModel.get(win.layoutIndex);
}

function currentLayout() {
    var win = rootWindow.activeLayoutWindow ? rootWindow.activeLayoutWindow : rootWindow;
    return win.layoutRepeater.itemAt(win.layoutIndex);
}

function parseOptions(str) {
    var obj = {};
    var regexp = /-([a-zA-Z0-9_\-:]+)\s([a-zA-Z0-9_\-.,:/]+)/g;
    var pairs = str.match(regexp);

    if (Array.isArray(pairs)) {
        for (var i = 0; i < pairs.length; ++i) {
            var arr = pairs[i].split(/\s/);
            obj[arr[0].slice(1)] = arr[1];
        }
    }

    return obj;
}

function stringifyOptions(obj) {
    var str = "";

    for (var key in obj) {
        if (typeof obj[key] === "string" || typeof obj[key] === "number") {
            str += "-%1 %2 ".arg(key).arg(obj[key]);
        }
    }

    return str.trim();
}

function ifLeftToRight(leftToRight, rightToLeft) {
    if (rightToLeft === undefined) {
        leftToRight = false;
    }

    return (Qt.application.layoutDirection == Qt.LeftToRight) ? leftToRight : rightToLeft;
}

function ifRightToLeft(rightToLeft, leftToRight) {
    if (leftToRight === undefined) {
        leftToRight = false;
    }

    return (Qt.application.layoutDirection == Qt.RightToLeft) ? rightToLeft : leftToRight;
}

// Objects
// NOTE: Shallow, not recursive "cloning"!
Object.defineProperty(Object, "assignDefault", {
                          enumerable: false,
                          configurable: true,
                          writable: true,
                          value: function(target) {
                              if (!(target instanceof Object)) {
                                  throw new TypeError("Cannot convert first argument to object");
                              }

                              for (var i = 1; i < arguments.length; ++i) {
                                  var source = arguments[i];

                                  if (!(source instanceof Object)) {
                                      continue;
                                  }

                                  for (var key in source) {
                                      if (target[key] === undefined) {
                                        target[key] = source[key];
                                      }
                                  }
                              }

                              return target;
                          }
                      });

// Strings
String.prototype.isEmpty = function() {
    return this.length === 0 || !this.trim();
};

String.prototype.leadingChars = function(fieldWidth, fillChar) {
    var chars = "";

    if (fillChar === undefined) {
        fillChar = " ";
    }

    for (var i = 0; i < fieldWidth - this.length; ++i) {
        chars += fillChar;
    }

    return chars + this;
}

// Math
function isNumeric(n) {
  return !isNaN(parseFloat(n)) && isFinite(n);
}

Number.prototype.clamp = function(min, max) {
    return Math.min(Math.max(this, min), max);
}

Number.prototype.inRange = function(min, max) {
    return (this >= min && val <= this) ? true : false;
}

// Other
//function sleep(ms) {
//  return new Promise(resolve => setTimeout(resolve, ms));
//}

// Debug
function log_info(message) {
    console.log(message);
}

function log_error(message) {
    console.log("ERROR: " + message);
}

function compareText(a, b) {
    var an = String(a || "").toLowerCase();
    var bn = String(b || "").toLowerCase();
    if (an < bn)
        return -1;
    if (an > bn)
        return 1;
    return 0;
}

function channelNumber(cam, fallback) {
    var c = parseInt(cam && (cam.channelId !== undefined ? cam.channelId : cam.channel));
    if (isNaN(c))
        c = parseInt(fallback);
    return isNaN(c) ? 0 : c;
}

// A recorder with one or two channels is a camera on its own. A dual-lens camera is still one device.
// A pasted stream is listed with the standalone cameras. More than two channels is an NVR.
function isStandaloneRecorder(rec) {
    if (!rec)
        return false;
    if (rec.kind === "rtsp" || rec.streamUrl)
        return true;
    var cams = rec.cameras || [];
    return cams.length <= 2;
}

// Each item: { standalone, name, channel, ip }.
// Standalone cameras first, A–Z. NVR cameras after them, by channel number.
function compareCameraChoice(a, b) {
    var as = !!(a && a.standalone);
    var bs = !!(b && b.standalone);
    if (as !== bs)
        return as ? -1 : 1;
    if (as) {
        var n = compareText(a && a.name, b && b.name);
        if (n !== 0)
            return n;
        return compareText(a && a.ip, b && b.ip);
    }
    var ac = parseInt(a && a.channel);
    var bc = parseInt(b && b.channel);
    if (isNaN(ac))
        ac = 0;
    if (isNaN(bc))
        bc = 0;
    if (ac !== bc)
        return ac - bc;
    var n2 = compareText(a && a.name, b && b.name);
    if (n2 !== 0)
        return n2;
    return compareText(a && a.ip, b && b.ip);
}

function sortCameraChoices(list) {
    var copy = [];
    var src = list || [];
    for (var i = 0; i < src.length; ++i)
        copy.push(src[i]);
    copy.sort(compareCameraChoice);
    return copy;
}

// A new array, ordered by channel number. The array passed in is left as it is.
function sortByChannel(cams) {
    var copy = [];
    var src = cams || [];
    for (var i = 0; i < src.length; ++i)
        copy.push(src[i]);
    copy.sort(function(a, b) {
        var ac = channelNumber(a, 0);
        var bc = channelNumber(b, 0);
        if (ac !== bc)
            return ac - bc;
        return compareText((a && (a.customName || a.name)) || "", (b && (b.customName || b.name)) || "");
    });
    return copy;
}

// Standalone cameras and pasted streams first, A–Z, then NVRs A–Z.
// Each recorder's cameras are copied into channel order. The saved list is not changed.
function sortRecordersForPicker(recs) {
    var standalone = [];
    var nvrs = [];
    var src = recs || [];
    for (var i = 0; i < src.length; ++i) {
        var rec = src[i];
        if (!rec)
            continue;
        var copy = {};
        for (var k in rec) {
            if (Object.prototype.hasOwnProperty.call(rec, k))
                copy[k] = rec[k];
        }
        copy.cameras = sortByChannel(rec.cameras || []);
        if (isStandaloneRecorder(rec))
            standalone.push(copy);
        else
            nvrs.push(copy);
    }
    function byRecorderName(a, b) {
        var n = compareText(a.name || a.ip, b.name || b.ip);
        if (n !== 0)
            return n;
        return compareText(a.ip, b.ip);
    }
    standalone.sort(byRecorderName);
    nvrs.sort(byRecorderName);
    return standalone.concat(nvrs);
}

function cleanMediaName(name) {
    var cleaned = String(name || "");
    cleaned = cleaned.replace(/\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}/g, "");
    cleaned = cleaned.replace(/([0-9a-fA-F]{1,4}:){7,7}[0-9a-fA-F]{1,4}/g, "");
    cleaned = cleaned.trim().replace(/^[_-\s]+|[_-\s]+$/g, "");
    cleaned = cleaned.replace(/ /g, "_").replace(/[^a-zA-Z0-9_\-\.]/g, "");
    return cleaned;
}

function snapshotSavePath(recorderName, channelId, cameraName, when) {
    var stamp = when instanceof Date ? when : new Date();
    var dayFolder = Qt.formatDateTime(stamp, "yyyy-MM-dd");
    var timeTag = Qt.formatDateTime(stamp, "hhmmss");
    var cleanRecName = cleanMediaName(recorderName);
    var cleanCamName = cleanMediaName(cameraName);
    if (cleanCamName === "")
        cleanCamName = "Camera";
    var channel = channelId !== undefined && channelId !== null && String(channelId) !== "" ? String(channelId) : "1";

    var filename = "";
    if (cleanRecName !== "")
        filename = cleanRecName + "_" + channel + "_" + cleanCamName + "_" + dayFolder + "_" + timeTag + ".jpg";
    else
        filename = channel + "_" + cleanCamName + "_" + dayFolder + "_" + timeTag + ".jpg";

    var rootPath = "";
    if (typeof rootWindow !== "undefined" && rootWindow.generalSettings && rootWindow.generalSettings.snapshotPath !== "")
        rootPath = Context.sanitizePlatformPath(rootWindow.generalSettings.snapshotPath, Context.defaultPicturesPath());
    else if (typeof generalSettings !== "undefined" && generalSettings.snapshotPath !== "")
        rootPath = Context.sanitizePlatformPath(generalSettings.snapshotPath, Context.defaultPicturesPath());
    else
        rootPath = Context.defaultPicturesPath();

    var folder = rootPath + "/" + dayFolder;
    Context.mkpath(folder);
    return folder + "/" + filename;
}

// The list of kinds of report that may show a frame, with "face" added once for a setting saved before the Face detection
// checkbox existed: such a setting kept face labels on by way of "other", so face is added only when "other" is still ticked
// (somebody who unticked it keeps face off). A list that already names face, or an empty one, comes back unchanged.
function kindsWithFace(kinds) {
    var items = String(kinds) === "" ? [] : String(kinds).split(",");
    if (items.indexOf("face") >= 0 || items.indexOf("other") < 0)
        return items.join(",");
    var at = items.indexOf("other");
    items.splice(at, 0, "face");
    return items.join(",");
}

