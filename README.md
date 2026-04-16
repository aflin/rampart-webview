# rampart-webview

A Rampart module that provides a cross-platform webview (native browser window)
for building desktop GUI applications with HTML, CSS, and JavaScript.

Uses the [webview](https://github.com/nicedoc/webview) library, which provides:
- **Linux**: GTK + WebKitGTK
- **macOS**: Cocoa + WebKit
- **Windows**: Win32 + WebView2

## Building

The build uses CMake to generate Makefiles.

### Prerequisites

**Linux (Debian/Ubuntu):**
```bash
sudo apt install cmake libgtk-3-dev libwebkit2gtk-4.1-dev
```

**macOS:** Xcode command line tools (WebKit is included with the OS) and CMake.

**Windows (MSYS2):** An MSYS2 environment with `cmake`, `gcc`, and `g++`.
WebView2 must be available on the target system (included with Windows 10/11
updates since late 2021).

### Build the module

The webview library is built automatically as a CMake subproject.

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

This produces `rampart-webview.so` and `test_webview.js` in the `build/`
directory.

### Install

```bash
make install
```

Installs to your Rampart modules directory (`/usr/local/rampart/modules/` by
default).

Alternatively, copy the `.so` file manually or run scripts from the directory
containing the `.so`.

## API

### Loading the Module

```javascript
var webview = require("rampart-webview");
```

### Creating a WebView

```javascript
var w = new webview.WebView({
    title:  "My App",          // Window title (default: "Rampart WebView")
    width:  800,               // Window width in pixels (default: 800)
    height: 600,               // Window height in pixels (default: 600)
    debug:  true,              // Enable browser developer tools (default: false)
    html:   "<h1>Hello</h1>",  // Initial HTML content
    // url: "https://example.com"  // OR navigate to a URL
});
```

All options are optional. If neither `html` nor `url` is provided, the window
opens with a blank page.

### Methods

#### w.setTitle(title)

Update the window title.

```javascript
w.setTitle("New Title");
```

#### w.setSize(width, height [, hint])

Update the window size. The optional `hint` parameter controls sizing behavior:

```javascript
w.setSize(1024, 768);           // Set default size
w.setSize(400, 300, "min");     // Set minimum size
w.setSize(1920, 1080, "max");   // Set maximum size
w.setSize(640, 480, "fixed");   // Set fixed (non-resizable) size
```

The hint can be a string (`"none"`, `"min"`, `"max"`, `"fixed"`) or a constant
(`webview.HINT_NONE`, `webview.HINT_MIN`, `webview.HINT_MAX`, `webview.HINT_FIXED`).

#### w.navigate(url)

Navigate to a URL.

```javascript
w.navigate("https://example.com");
```

#### w.setHtml(html)

Load HTML content directly.

```javascript
w.setHtml("<h1>Hello World</h1>");
```

#### w.init(js)

Inject JavaScript that will execute automatically before every page load.
Useful for setting up global functions or polyfills.

```javascript
w.init("window.myGlobal = 'available on every page';");
```

#### w.eval(js)

Evaluate JavaScript in the webview immediately.

```javascript
w.eval("document.title = 'Changed from Rampart';");
```

#### w.bind(name, callback)

Bind a Rampart (Duktape) function so it can be called from JavaScript running
inside the webview. The bound function appears as `window.<name>()` in the
webview and returns a Promise.

```javascript
w.bind("add", function(a, b) {
    return a + b;
});
```

From within the webview's HTML/JavaScript:

```html
<script>
async function doAdd() {
    var result = await window.add(3, 4);
    console.log(result); // 7
}
</script>
```

The callback can accept and return rich types including numbers, strings,
objects, arrays, booleans, null, Dates, Buffers/ArrayBuffers, TypedArrays,
RegExps, Maps, Sets, NaN, Infinity, and undefined.  Cyclic object references
are also preserved.  If the callback throws an error, the Promise in the
webview is rejected with the error message.

#### w.unbind(name)

Remove a previously bound function.

```javascript
w.unbind("add");
```

#### w.run()

Start the webview event loop. **This call blocks** until the window is closed
(either by the user or by calling `w.terminate()` from a bound callback).

All setup (bind, init, setHtml, navigate, etc.) must be done before calling
`run()`.

```javascript
w.run();
// Execution resumes here after the window is closed
```

#### w.terminate()

Stop the event loop, causing `w.run()` to return. Typically called from within
a bound callback:

```javascript
w.bind("quit", function() {
    w.terminate();
});
```

#### w.destroy()

Explicitly destroy the webview instance and free resources. Also called
automatically by the garbage collector if the object goes out of scope.

```javascript
w.destroy();
```

### Constants

| Constant | Value | Description |
|---|---|---|
| `webview.HINT_NONE` | 0 | Default window size |
| `webview.HINT_MIN` | 1 | Minimum size constraint |
| `webview.HINT_MAX` | 2 | Maximum size constraint |
| `webview.HINT_FIXED` | 3 | Fixed (non-resizable) |

## Accessing Local Servers

If your webview page needs to access a local HTTP server (e.g.,
`http://127.0.0.1:8088/api/data.json`), browser same-origin policy (CORS)
may block the request depending on how the page was loaded. Pages loaded via
`setHtml()` or `file://` URLs have a `null` origin, so fetch/XHR requests to
`http://` URLs are considered cross-origin.

There are three ways to handle this:

### Option 1: Serve the page from the same server

If you're running rampart-server, navigate to it directly. Since the page and
API share the same origin, no CORS issues arise.

```javascript
w.navigate("http://127.0.0.1:8088/myapp/index.html");
```

### Option 2: Add CORS headers on the server

Configure your local server to include permissive CORS headers. This allows
pages loaded via `setHtml()` or `file://` to make requests to the server.

```javascript
// In your rampart-server route handler:
req.header("Access-Control-Allow-Origin", "*");
```

### Option 3: Use w.bind() as a proxy

Use `w.bind()` to expose a Rampart function that performs the HTTP request
server-side, bypassing browser security restrictions entirely. This is the most
flexible approach and works regardless of how the page was loaded.

```javascript
// Rampart side
var curl = require("rampart-curl");

w.bind("fetchJson", function(url) {
    var res = curl.fetch(url);
    return JSON.parse(res.body);
});
```

```html
<!-- Webview side -->
<script>
async function loadData() {
    var data = await window.fetchJson("http://127.0.0.1:8088/apps/myapp.json");
    console.log(data);
}
</script>
```

The HTTP request happens in Rampart (no browser restrictions), and the result
is returned to the webview as a resolved Promise.

## Headless JavaScriptCore

This module also provides direct access to the JavaScriptCore (JSC) engine
that ships with WebKitGTK, allowing you to execute modern JavaScript (ES2020+)
without a GUI.  This gives Rampart access to a full JIT-compiled JS engine for
running third-party libraries that require features beyond Duktape's ES5.1
support.

There are two interfaces: `jscExec()` for one-shot evaluation, and
`JSCContext` for a persistent interpreter where you can load modules, maintain
state, and call methods on JSC objects directly from Rampart.

**Compatibility note:** The JSC context is a pure JavaScript engine — it
provides ECMAScript builtins (Math, JSON, Date, Promise, Map, Set, RegExp,
TypedArrays, etc.) but does *not* include Web Platform APIs (`fetch`,
`setTimeout`, `WebSocket`, `localStorage`, DOM, etc.) or Node.js APIs (`fs`,
`http`, `require`, etc.).  Promises and `async`/`await` work correctly (the
microtask queue drains between calls).

As a general rule: if a library's job is **transforming data** rather than doing
I/O or rendering, it will work.  The following libraries have been tested and
verified: lodash, mathjs, marked, ajv, papaparse, handlebars, js-yaml, fuse.js,
and validator.js.  Libraries that need network access, timers, a DOM, or Node.js
modules will not work.  Use Rampart's own modules (rampart-curl, rampart-server,
rampart-lmdb, etc.) for I/O and pass data into JSC via `jsc.set()` for
processing.

**Note on callbacks:** Duktape functions cannot be called from within JSC —
they are in different engines.  You can pass data values freely in both
directions, but operations that require a callback function as an argument
must be done inside `jsc.eval()` where the callback is defined as a JSC
function.  Calling JSC methods with simple arguments (strings, numbers,
objects, arrays) works directly from Rampart.

### webview.jscExec(code)

Evaluate JavaScript in a temporary JSC context and return the result.  A new
context is created and destroyed on each call.  The return value is
deep-converted to a native Duktape value with rich type mapping:

| JSC type | Duktape result |
|---|---|
| number, string, boolean, null, undefined | Native equivalent (including NaN, Infinity) |
| Date | Duktape Date (`instanceof Date` works) |
| ArrayBuffer | Node.js-style Buffer |
| TypedArray (Uint8Array, Float64Array, etc.) | Matching Duktape TypedArray |
| RegExp | Duktape RegExp (source + flags preserved) |
| Error / TypeError / etc. | Duktape Error (name + message preserved) |
| Map | Array of `[key, value]` pairs |
| Set | Array of unique values |
| Array, Object | Recursive deep conversion |
| Function | Its `toString()` source text |

```javascript
var webview = require("rampart-webview");

webview.jscExec("40 + 2");                          // 42
webview.jscExec("new Date('2026-01-01')");           // Date object
webview.jscExec("new Uint8Array([0xCA, 0xFE])");     // TypedArray
webview.jscExec("/^hello$/gi");                       // RegExp
webview.jscExec("[...new Set([1,2,2,3])]");           // [1, 2, 3]
```

Thrown exceptions propagate as Duktape errors:

```javascript
try {
    webview.jscExec("throw new TypeError('oops')");
} catch(e) {
    console.log(e.message); // contains "TypeError: oops"
}
```

### new webview.JSCContext()

Create a persistent JavaScriptCore context.  State is preserved across calls,
and objects returned from JSC are wrapped in Duktape proxies so you can access
their properties and call their methods directly.

```javascript
var jsc = new webview.JSCContext();
```

#### jsc.eval(code)

Evaluate JavaScript code in the persistent context.  Returns the value of the
last expression.  Primitives, Dates, RegExps, and Buffers are auto-converted to
native Duktape values.  Objects and functions are returned as live proxies backed
by the JSC engine.

```javascript
jsc.eval("var x = 10;");
jsc.eval("x * 4");            // 40 (state persists)

var obj = jsc.eval("({greet: function(n) { return 'Hello ' + n; }})");
obj.greet("World");            // "Hello World"
```

#### jsc.loadScript(path)

Read a JavaScript file and evaluate it in the persistent context.  Returns
the value of the last expression (usually not meaningful for library scripts).
Use `getGlobal()` to access globals that the script defines.

```javascript
jsc.loadScript("/path/to/library.js");
var lib = jsc.getGlobal("LibraryName");
```

#### jsc.require(path)

Load a JavaScript file with a CommonJS-compatible shim.  The file is wrapped in
a `function(module, exports, require){ ... }` closure and `module.exports` is
returned.  This works with UMD bundles and CommonJS modules, which covers most
npm packages.  There is also limited, experimental support for ES module syntax
(`export default`, `export function`, `export { ... }`, etc.) — if the CommonJS
shim fails with a SyntaxError, `require` will attempt to transform `export`
statements automatically.

Both CommonJS and ESM support expect **single-file bundles**.  Multi-file
modules with `import` or `require` dependencies between files are not resolved.
Use a bundler (e.g., `esbuild lib.js --bundle --format=cjs`) to produce a
single file first.

```javascript
var math = jsc.require("/path/to/math.js");
math.add(2, 3);  // 5
```

#### jsc.set(name, value)

Set a global variable in the JSC context.  The value is converted from Duktape
to JSC (numbers, strings, booleans, arrays, objects, Buffers, Dates, and
already-wrapped JSC values are all supported).

```javascript
jsc.set("config", {debug: true, maxRetries: 3});
jsc.eval("config.maxRetries");  // 3
```

#### jsc.getGlobal(name)

Get a global variable from the JSC context as a wrapped value.

```javascript
jsc.loadScript("library.js");
var lib = jsc.getGlobal("Library");
lib.someMethod();
```

#### jsc.destroy()

Destroy the JSC context and free resources.  Also called automatically by the
garbage collector.

```javascript
jsc.destroy();
```

### Working with Wrapped JSC Objects

Objects returned from `eval()`, `require()`, `getGlobal()`, or JSC method calls
are Duktape proxies that resolve properties lazily from JSC:

```javascript
var math = jsc.require("math.js");

// Property access resolves from JSC
math.pi;                       // 3.141592653589793

// Method calls marshal arguments to JSC and convert the result
math.sqrt(144);                // 12
math.add(3, 4);                // 7

// Returned objects are also proxied
var m = math.matrix([[1, 2], [3, 4]]);
math.det(m);                   // -2
math.inv(m).toString();        // "[[-2, 1], [1.5, -0.5]]"
```

JSC objects passed as arguments to other JSC functions are unwrapped
automatically — no manual conversion is needed:

```javascript
var c1 = math.complex(3, 4);
var c2 = math.complex(1, -2);
math.add(c1, c2).toString();   // "4 + 2i"
```

#### .toValue()

Deep-convert a wrapped JSC object to a plain Duktape value (using the same
rich type mapping as `jscExec()`).  Useful when you need a native object for
`JSON.stringify()`, passing to other Rampart modules, etc.

```javascript
var result = math.evaluate("[1, 2, 3]");
var arr = result.toValue();     // plain Duktape array [1, 2, 3]
JSON.stringify(arr);            // "[1,2,3]"
```

#### .toString()

Get the string representation of a wrapped JSC value.

```javascript
math.matrix([[1, 0], [0, 1]]).toString();  // "[[1, 0], [0, 1]]"
```

## Examples

### WebView: Counter App

```javascript
var webview = require("rampart-webview");

var w = new webview.WebView({
    title: "Counter App",
    width: 400,
    height: 300,
    debug: false,
    html: '<html>\
<body style="font-family: sans-serif; text-align: center; padding: 40px;">\
  <h1 id="count">0</h1>\
  <button onclick="increment()">Increment</button>\
  <button onclick="window.quit()">Quit</button>\
  <script>\
    var count = 0;\
    async function increment() {\
      count = await window.addOne(count);\
      document.getElementById("count").textContent = count;\
    }\
  </script>\
</body>\
</html>'
});

w.bind("addOne", function(n) {
    console.log("addOne called:", n);
    return n + 1;
});

w.bind("quit", function() {
    w.terminate();
});

w.run();
w.destroy();
```

### JSCContext: Fetching and Using a JS Library

This example uses `rampart-curl` to download mathjs from a CDN, saves it
to a file, then loads it into a JSCContext and runs several operations.

```javascript
rampart.globalize(rampart.utils);
var curl = require("rampart-curl");
var wv   = require("rampart-webview");

var url  = "https://cdn.jsdelivr.net/npm/mathjs@13.2.2/lib/browser/math.js";
var file = "/tmp/math.js";

/* Fetch the library */
printf("Fetching mathjs from CDN... ");
var res = curl.fetch({location: true}, url);
if (res.status !== 200) {
    printf("FAILED (status %d)\n", res.status);
    process.exit(1);
}
printf("%d bytes, status %d\n", res.body.length, res.status);

/* Save to file */
rampart.utils.fprintf(file, "%s", res.body);
printf("Saved to %s\n\n", file);

/* Load into JSC and run some tests */
var jsc  = new wv.JSCContext();
var math = jsc.require(file);

printf("mathjs version: %s\n\n", math.version);

/* Linear algebra */
printf("Linear algebra:\n");
var m = math.matrix([[2, 1, 0], [1, 3, 1], [0, 1, 2]]);
printf("  matrix:       %s\n", m.toString());
printf("  determinant:  %s\n", math.det(m));
printf("  inverse:      %s\n", math.inv(m).toString());

/* Expression parser */
printf("\nExpression parser:\n");
printf("  e^(i*pi) + 1 = %s\n", math.evaluate("e^(i*pi) + 1").toString());
printf("  3 inches in cm = %s\n", math.evaluate("3 inch to cm").toString());
printf("  derivative of sin(x)*x^2 = %s\n",
    math.derivative("sin(x)*x^2", "x").toString());

/* Statistics on data passed from Duktape */
printf("\nStatistics (data passed from Duktape to JSC):\n");
var data = [2, 4, 6, 8, 10, 12, 14, 16, 18, 20];
printf("  data:     %s\n", JSON.stringify(data));
printf("  mean:     %s\n", math.mean(data));
printf("  median:   %s\n", math.median(data));
printf("  std:      %s\n", math.std(data));

jsc.destroy();
printf("\nDone.\n");
```

## License

MIT
