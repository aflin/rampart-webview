rampart.globalize(rampart.utils);
var webview = require("rampart-webview");

/* Cookies and getCookies require a real URL scheme (http://, https://, file://).
   We load an initial example.com page so cookies can be set.  The test page
   is served via setHtml once the initial navigation is complete. */

var TEST_URL = "https://example.com/";

var w = new webview.WebView({
    title: "Rampart WebView Test",
    width: 800,
    height: 700,
    debug: true
});

/* ---- Bindings ---- */

w.bind("add", function(a, b) {
    console.log("add() called with", a, b);
    return a + b;
});

w.bind("greet", function(name) {
    console.log("greet() called with", name);
    return "Hello, " + name + "!";
});

w.bind("quit", function() {
    console.log("quit() called");
    w.terminate();
});

/* Snapshot binding (Linux/macOS) */
if (typeof w.snapshot === "function") {
    w.bind("snapshot", function() {
        try {
            var png = w.snapshot();
            var path = "/tmp/webview_test_snapshot.png";
            fprintf(path, "%s", png);
            return "Saved " + png.length + " bytes to " + path;
        } catch(e) { return "Error: " + e.message; }
    });
} else {
    w.bind("snapshot", function() { return "not available on this platform"; });
}

/* Cookie bindings */
if (typeof w.setCookie === "function") {
    w.bind("setCookies", function() {
        try {
            w.setCookie("session", "abc123");
            w.setCookie("user", "alice");
            w.setCookie("theme", "dark");
            return "Set 3 cookies";
        } catch(e) { return "Error: " + e.message; }
    });
    w.bind("getAllCookies", function() {
        try {
            return JSON.stringify(w.getCookies(), null, 2);
        } catch(e) { return "Error: " + e.message; }
    });
} else {
    w.bind("setCookies",   function() { return "not available on this platform"; });
    w.bind("getAllCookies", function() { return "not available on this platform"; });
}

/* getContents binding (all platforms) */
w.bind("fetchContents", function() {
    return new Promise(function(resolve) {
        w.getContents(function(html, err) {
            if (err) resolve("Error: " + err.message);
            else resolve("HTML length: " + html.length + " bytes");
        });
    });
});

/* Eval with callback — test different value types */
w.bind("runEvalTests", function() {
    return new Promise(function(resolve) {
        var out = [];
        w.eval("document.title", function(r, e) {
            out.push("title: " + r);
            w.eval("2+2+5", function(r, e) {
                out.push("math: " + r);
                w.eval("new Date('2026-01-01').getTime()", function(r, e) {
                    out.push("date (ms): " + r);
                    w.eval("Promise.resolve(['a','b','c'])", function(r, e) {
                        out.push("promise array: " + JSON.stringify(r));
                        w.eval("throw new Error('test error')", function(r, e) {
                            out.push("error caught: " + (e ? e.message : "no error"));
                            resolve(out.join("\n"));
                        });
                    });
                });
            });
        });
    });
});

/* User-Agent binding */
if (typeof w.setUserAgent === "function") {
    w.bind("setCustomUA", function(ua) {
        try {
            w.setUserAgent(ua);
            return "User agent set to: " + ua;
        } catch(e) { return "Error: " + e.message; }
    });
} else {
    w.bind("setCustomUA", function() { return "not available on this platform"; });
}

/* ---- Event subscriptions (must be set up before first load) ---- */

var consoleCount = 0;
w.on("console", function(level, args) {
    consoleCount++;
    console.log("[page " + level + "]", args.join(" "));
});

w.on("load", function(url) {
    console.log("[load event]", url);

    /* Once the initial page has loaded, swap in the test HTML */
    if (url === TEST_URL || url.indexOf("example.com") !== -1) {
        w.setHtml(TEST_HTML);
    }
});

/* Set a custom UA to verify setUserAgent works on the first request */
if (typeof w.setUserAgent === "function") {
    w.setUserAgent("RampartWebView/1.0 (test)");
}

/* ---- Test HTML ---- */

var TEST_HTML = '<html>\
<head><title>Rampart WebView Test</title></head>\
<body style="font-family: sans-serif; padding: 20px; max-width: 760px">\
<h1>Rampart WebView — full test</h1>\
<p>Click each button to test the corresponding API.</p>\
\
<h3>1. bind() — Rampart functions callable from JS</h3>\
<button onclick="t(\'add\', window.add(3,4), \'add-r\')">add(3,4)</button>\
<button onclick="t(\'greet\', window.greet(\'World\'), \'greet-r\')">greet("World")</button>\
<div id="add-r" class="r"></div><div id="greet-r" class="r"></div>\
\
<h3>2. snapshot() — capture the webview as PNG</h3>\
<button onclick="t(\'snapshot\', window.snapshot(), \'snap-r\')">Snapshot</button>\
<div id="snap-r" class="r"></div>\
\
<h3>3. getContents(callback) — fetch current DOM as HTML</h3>\
<button onclick="t(\'getContents\', window.fetchContents(), \'cont-r\')">Get Contents</button>\
<div id="cont-r" class="r"></div>\
\
<h3>4. eval(code, callback) — eval with captured return value</h3>\
<button onclick="t(\'eval\', window.runEvalTests(), \'eval-r\')">Run eval tests</button>\
<pre id="eval-r" class="r"></pre>\
\
<h3>5. setUserAgent() / navigator.userAgent</h3>\
<div>Current: <code id="ua-current"></code></div>\
<button onclick="changeUA()">Set custom UA</button>\
<div id="ua-r" class="r"></div>\
\
<h3>6. setCookie() / getCookies() (Linux/macOS only, needs real URL)</h3>\
<button onclick="t(\'setCookie\', window.setCookies(), \'ck-set-r\')">Set cookies</button>\
<button onclick="t(\'getCookies\', window.getAllCookies(), \'ck-get-r\')">Get cookies</button>\
<div id="ck-set-r" class="r"></div>\
<pre id="ck-get-r" class="r"></pre>\
\
<h3>7. on("console") — verify console output reaches Rampart</h3>\
<button onclick="sendConsole()">Generate console output</button>\
<div id="c-r" class="r"></div>\
\
<hr>\
<button onclick="window.quit()" style="margin-top:20px; padding:10px 20px; font-size:16px">Quit</button>\
\
<style>\
  .r { margin: 6px 0 12px 0; padding: 8px; background: #f4f4f4; border-radius: 4px; font-family: monospace; white-space: pre-wrap; min-height: 1em }\
  button { margin: 2px 4px 2px 0 }\
  h3 { margin-top: 18px }\
</style>\
\
<script>\
  document.getElementById("ua-current").textContent = navigator.userAgent;\
\
  async function t(name, p, elId) {\
    var el = document.getElementById(elId);\
    el.textContent = "...";\
    try {\
      var r = await p;\
      el.textContent = (typeof r === "string") ? r : JSON.stringify(r);\
    } catch(e) {\
      el.textContent = "Error: " + e.message;\
    }\
  }\
\
  async function changeUA() {\
    var ua = "CustomAgent/3.14 (rampart-webview test)";\
    var r = await window.setCustomUA(ua);\
    document.getElementById("ua-r").textContent =\
      r + " — (page already loaded, navigator.userAgent will reflect on next navigation)";\
  }\
\
  function sendConsole() {\
    console.log("test log message");\
    console.warn("test warning");\
    console.error("test error");\
    console.info("test info");\
    document.getElementById("c-r").textContent =\
      "Sent 4 messages — check Rampart terminal output";\
  }\
</script>\
</body>\
</html>';

console.log("Starting webview...");
console.log("Navigating to", TEST_URL, "first (for cookie support)...");

w.navigate(TEST_URL);
w.run();

w.destroy();
console.log("Webview closed.  Total console events captured:", consoleCount);
