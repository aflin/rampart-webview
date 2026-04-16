rampart.globalize(rampart.utils);
var webview = require("rampart-webview");

/* Cookies need a real URL scheme.  We navigate to example.com first,
   then setHtml replaces the page content once load fires. */
var TEST_URL = "https://example.com/";

var w = new webview.WebView({
    title: "Rampart WebView Test",
    width: 820,
    height: 720,
    debug: true
});

/* ---------- helper: write a string to a page element via eval ---------- */

function setElementText(id, text) {
    w.eval(
        "var __e = document.getElementById(" + JSON.stringify(id) + ");" +
        "if (__e) __e.textContent = " + JSON.stringify(text) + ";"
    );
}

/* ---------- Bindings ---------- */

w.bind("add",   function(a, b) { return a + b; });
w.bind("greet", function(name) { return "Hello, " + name + "!"; });
w.bind("quit",  function() { w.terminate(); });

/* Snapshot — synchronous, returns the result directly */
w.bind("snapshot", function() {
    if (typeof w.snapshot !== "function")
        return "snapshot() not available on this platform";
    try {
        var png = w.snapshot();
        var path = "/tmp/webview_test_snapshot.png";
        fprintf(path, "%s", png);
        return "Saved " + png.length + " bytes to " + path;
    } catch(e) { return "Error: " + e.message; }
});

/* Async: bind triggers the op, result is written to a DOM element
   via eval when ready.  This avoids returning Promises from bind. */

w.bind("fetchContents", function() {
    w.getContents(function(html, err) {
        var msg = err ? ("Error: " + err.message)
                      : ("HTML length: " + html.length + " bytes");
        setElementText("cont-r", msg);
    });
    return "fetching...";
});

w.bind("runEvalTests", function() {
    var results = [];
    var tests = [
        ["document.title",          "title"],
        ["2+2+5",                   "math"],
        ["new Date(0).toISOString()", "date"],
        ["[1,2,3].reduce((a,b)=>a+b)", "reduce"],
        ["throw new Error('intentional')", "error"]
    ];
    var i = 0;
    function next() {
        if (i >= tests.length) {
            setElementText("eval-r", results.join("\n"));
            return;
        }
        var t = tests[i++];
        w.eval(t[0], function(r, err) {
            results.push(t[1] + ": " +
                (err ? "[error] " + err.message : JSON.stringify(r)));
            next();
        });
    }
    next();
    return "running...";
});

/* Simple demonstration of w.eval(code, callback):
   browser fetches a URL, Rampart receives the HTML in a plain callback.
   Uses Promises in the BROWSER (not Rampart) — the eval wrapper awaits
   them and passes the resolved value to the Rampart callback. */
w.bind("fetchURL", function(url) {
    w.eval(
        "fetch(" + JSON.stringify(url) + ").then(function(r){return r.text()})",
        function(html, err) {
            if (err) {
                setElementText("fetch-r", "Error: " + err.message);
                return;
            }
            var path = "/tmp/webview_fetched.html";
            fprintf(path, "%s", html);
            setElementText("fetch-r",
                "Fetched " + html.length + " bytes, saved to " + path);
        }
    );
    return "fetching...";
});

/* Cookies — synchronous */
w.bind("setCookies", function() {
    if (typeof w.setCookie !== "function")
        return "setCookie() not available on this platform";
    try {
        w.setCookie("session", "abc123");
        w.setCookie("user",    "alice");
        w.setCookie("theme",   "dark");
        return "Set 3 cookies";
    } catch(e) { return "Error: " + e.message; }
});

w.bind("getAllCookies", function() {
    if (typeof w.getCookies !== "function")
        return "getCookies() not available on this platform";
    try {
        var c = w.getCookies();
        return JSON.stringify(c, null, 2);
    } catch(e) { return "Error: " + e.message; }
});

/* User Agent — synchronous */
w.bind("setCustomUA", function(ua) {
    if (typeof w.setUserAgent !== "function")
        return "setUserAgent() not available on this platform";
    try {
        w.setUserAgent(ua);
        return "UA updated to: " + ua +
               " (effective on next navigation; current page keeps old UA)";
    } catch(e) { return "Error: " + e.message; }
});

/* ---------- Events — must be registered before first load ---------- */

var consoleCount = 0;
w.on("console", function(level, args) {
    consoleCount++;
    console.log("[page " + level + "]", args.join(" "));
});

w.on("load", function(url) {
    console.log("[load event]", url);
    /* When the initial real URL finishes loading, inject our test page */
    if (url && url.indexOf("example.com") !== -1) {
        w.setHtml(TEST_HTML);
    }
});

if (typeof w.setUserAgent === "function")
    w.setUserAgent("RampartWebView/1.0 (test)");

/* ---------- Test HTML ---------- */

var TEST_HTML =
'<html><head><title>Rampart WebView Test</title></head>' +
'<body style="font-family:sans-serif;padding:20px;max-width:780px">' +
'<h1>Rampart WebView — full test</h1>' +
'<p>Click each button to test the corresponding API.</p>' +

'<h3>1. bind() — Rampart functions</h3>' +
'<button onclick="run(window.add(3,4), \'add-r\')">add(3,4)</button>' +
'<button onclick="run(window.greet(\'World\'), \'greet-r\')">greet("World")</button>' +
'<div id="add-r" class="r"></div><div id="greet-r" class="r"></div>' +

'<h3>2. snapshot() — capture webview as PNG</h3>' +
'<button onclick="run(window.snapshot(), \'snap-r\')">Snapshot</button>' +
'<div id="snap-r" class="r"></div>' +

'<h3>3. getContents(callback) — fetch current DOM</h3>' +
'<button onclick="run(window.fetchContents(), \'cont-r\')">Get Contents</button>' +
'<div id="cont-r" class="r">(click to fetch)</div>' +

'<h3>4. eval(code, callback) — eval with captured result</h3>' +
'<button onclick="run(window.runEvalTests(), \'eval-r\')">Run eval tests</button>' +
'<pre id="eval-r" class="r">(click to run)</pre>' +

'<h3>4b. eval() + fetch — browser fetches URL, Rampart gets HTML in callback</h3>' +
'<input id="fetch-url" value="https://httpbin.org/html" style="width:320px">' +
'<button onclick="run(window.fetchURL(document.getElementById(\'fetch-url\').value), \'fetch-r\')">Fetch</button>' +
'<div id="fetch-r" class="r">(URL html is saved to /tmp/webview_fetched.html)</div>' +

'<h3>5. setUserAgent() / navigator.userAgent</h3>' +
'<div>Current: <code id="ua-current"></code></div>' +
'<button onclick="changeUA()">Set custom UA</button>' +
'<div id="ua-r" class="r"></div>' +

'<h3>6. setCookie() / getCookies()</h3>' +
'<button onclick="run(window.setCookies(), \'ck-set-r\')">Set cookies</button>' +
'<button onclick="run(window.getAllCookies(), \'ck-get-r\')">Get cookies</button>' +
'<div id="ck-set-r" class="r"></div>' +
'<pre id="ck-get-r" class="r"></pre>' +

'<h3>7. on("console") — console output reaches Rampart</h3>' +
'<button onclick="sendConsole()">Generate console output</button>' +
'<div id="c-r" class="r"></div>' +

'<hr><button onclick="window.quit()" style="margin-top:20px;padding:10px 20px;font-size:16px">Quit</button>' +

'<style>' +
'.r{margin:6px 0 12px 0;padding:8px;background:#f4f4f4;border-radius:4px;font-family:monospace;white-space:pre-wrap;min-height:1em}' +
'button{margin:2px 4px 2px 0}' +
'h3{margin-top:18px}' +
'</style>' +

'<script>' +
'document.getElementById("ua-current").textContent=navigator.userAgent;' +

/* Bindings return a Promise (from the webview protocol).  We await it
   and stringify the result.  This is straightforward because the Rampart
   side returns simple scalar values (not Promises). */
'async function run(p,elId){' +
  'var el=document.getElementById(elId);' +
  'el.textContent="...";' +
  'try{' +
    'var r=await p;' +
    'el.textContent=(typeof r==="string")?r:JSON.stringify(r);' +
  '}catch(e){' +
    'el.textContent="Error: "+(e&&e.message?e.message:String(e));' +
  '}' +
'}' +

'async function changeUA(){' +
  'var r=await window.setCustomUA("CustomAgent/3.14 (rampart-webview test)");' +
  'document.getElementById("ua-r").textContent=r;' +
'}' +

'function sendConsole(){' +
  'console.log("test log message");' +
  'console.warn("test warning");' +
  'console.error("test error");' +
  'console.info("test info");' +
  'document.getElementById("c-r").textContent="Sent 4 console messages — check Rampart terminal";' +
'}' +
'</script>' +
'</body></html>';

console.log("Starting webview...");
console.log("Navigating to", TEST_URL, "first (for cookie support)...");

w.navigate(TEST_URL);
w.run();

w.destroy();
console.log("Webview closed. Console events captured:", consoleCount);
