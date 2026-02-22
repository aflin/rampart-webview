var webview = require("rampart-webview");

var w = new webview.WebView({
    title: "Rampart WebView Test",
    width: 800,
    height: 600,
    debug: true,
    html: '<html>\
<body style="font-family: sans-serif; padding: 20px;">\
  <h1>Rampart WebView</h1>\
  <p>Testing the rampart-webview module.</p>\
  <hr>\
  <h3>Bind Test</h3>\
  <button id="btn" onclick="testAdd()">Call add(3, 4)</button>\
  <div id="result" style="margin-top:10px; font-size:18px; color:blue;"></div>\
  <hr>\
  <h3>String Test</h3>\
  <button onclick="testGreet()">Call greet("World")</button>\
  <div id="greet-result" style="margin-top:10px; font-size:18px; color:green;"></div>\
  <hr>\
  <button onclick="window.quit()" style="margin-top:20px; padding:10px 20px; font-size:16px;">Quit</button>\
  <script>\
    async function testAdd() {\
      try {\
        var res = await window.add(3, 4);\
        document.getElementById("result").textContent = "add(3, 4) = " + JSON.stringify(res);\
      } catch(e) {\
        document.getElementById("result").textContent = "Error: " + e;\
      }\
    }\
    async function testGreet() {\
      try {\
        var res = await window.greet("World");\
        document.getElementById("greet-result").textContent = res;\
      } catch(e) {\
        document.getElementById("greet-result").textContent = "Error: " + e;\
      }\
    }\
  </script>\
</body>\
</html>'
});

// Bind a function that adds two numbers
w.bind("add", function(a, b) {
    console.log("add() called with", a, b);
    return a + b;
});

// Bind a function that returns a string
w.bind("greet", function(name) {
    console.log("greet() called with", name);
    return "Hello, " + name + "!";
});

// Bind a quit function
w.bind("quit", function() {
    console.log("quit() called");
    w.terminate();
});

console.log("Starting webview...");
w.run();

w.destroy();
console.log("Webview closed.");
