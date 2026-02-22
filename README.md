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

The callback can return any JSON-serializable value (numbers, strings, objects,
arrays, booleans, null). If the callback throws an error, the Promise in the
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

## Example

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

## License

MIT
