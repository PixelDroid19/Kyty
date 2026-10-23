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

function formatMinutes(ms) {
    var date = new Date(ms);
    return Qt.formatDateTime(date, "d MMM");
}
