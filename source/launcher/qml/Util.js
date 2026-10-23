.pragma library

// Turns a file:// URL from a FileDialog into the path the backend expects.
function localPath(url) {
    var text = url.toString();
    if (text.indexOf("file:///") !== 0)
        return text;
    text = decodeURIComponent(text.substring(7));
    // Windows drive letters arrive as /C:/...
    if (/^\/[A-Za-z]:/.test(text))
        text = text.substring(1);
    return text;
}

function initials(title) {
    var words = (title || "").trim().split(/\s+/);
    var letters = "";
    for (var i = 0; i < words.length && letters.length < 2; i++) {
        if (words[i].length > 0)
            letters += words[i].charAt(0);
    }
    return letters.toUpperCase();
}

function formatMinutes(ms) {
    var date = new Date(ms);
    return Qt.formatDateTime(date, "d MMM");
}

// The same artwork cut to the size it is drawn at, with rounded corners. The provider rounds it once, off the
// UI thread, and keeps the result on disk.
function rounded(url) {
    return url ? url.replace("image://art/", "image://art/round/") : "";
}
