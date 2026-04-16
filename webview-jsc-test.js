/* make printf et. al. global */
rampart.globalize(rampart.utils);

if (rampart.buildPlatform.indexOf("windows") !== -1 ||
    rampart.buildPlatform.indexOf("MSYS") !== -1) {
    printf("JSC tests skipped: JavaScriptCore is not available on Windows.\n");
    process.exit(0);
}

var wv = require("rampart-webview");

var _nfailed = 0;

function testFeature(name, test)
{
    var error=false;
    printf("testing %-60s - ", name);
    fflush(stdout);
    if (typeof test =='function'){
        try {
            test=test();
        } catch(e) {
            error=e;
            test=false;
        }
    }
    if(test)
        printf("passed\n")
    else
    {
        printf(">>>>> FAILED <<<<<\n");
        _nfailed++;
    }
    if(error) console.log(error);
}


/* ================================================================
   jscExec — one-shot evaluation
   ================================================================ */

testFeature("jscExec - number", function() {
    return wv.jscExec("40 + 2") === 42;
});

testFeature("jscExec - string", function() {
    return wv.jscExec("'hello' + ' ' + 'world'") === "hello world";
});

testFeature("jscExec - boolean/null/undefined", function() {
    return wv.jscExec("true") === true
        && wv.jscExec("null") === null
        && wv.jscExec("undefined") === undefined;
});

testFeature("jscExec - Date", function() {
    var d = wv.jscExec("new Date('2026-04-15T12:30:00Z')");
    return d instanceof Date && d.getFullYear() === 2026;
});

testFeature("jscExec - RegExp", function() {
    var re = wv.jscExec("/^hello$/gi");
    return re instanceof RegExp && re.test("Hello");
});

testFeature("jscExec - TypedArray", function() {
    var u8 = wv.jscExec("new Uint8Array([10, 20, 30])");
    return u8[0] === 10 && u8[1] === 20 && u8[2] === 30;
});

testFeature("jscExec - ArrayBuffer", function() {
    var buf = wv.jscExec("new ArrayBuffer(8)");
    return Buffer.isBuffer(buf) && buf.length === 8;
});

testFeature("jscExec - Map", function() {
    var m = wv.jscExec("new Map([['a',1],['b',2]])");
    return m instanceof Map && m.size === 2
        && m.get("a") === 1 && m.get("b") === 2;
});

testFeature("jscExec - Set", function() {
    var s = wv.jscExec("new Set([10,20,30,10])");
    return s instanceof Set && s.size === 3
        && s.has(10) && s.has(30) && !s.has(99);
});

testFeature("jscExec - nested object", function() {
    var o = wv.jscExec("({a: 1, b: {c: [2, 3]}})");
    return o.a === 1 && o.b.c[0] === 2 && o.b.c[1] === 3;
});

testFeature("jscExec - exception", function() {
    try {
        wv.jscExec("throw new TypeError('boom')");
        return false;
    } catch(e) {
        return e.message.indexOf("TypeError") !== -1
            && e.message.indexOf("boom") !== -1;
    }
});

testFeature("jscExec - syntax error", function() {
    try {
        wv.jscExec("not valid !!!");
        return false;
    } catch(e) {
        return e.message.indexOf("SyntaxError") !== -1;
    }
});


/* ================================================================
   JSCContext — persistent context
   ================================================================ */

var jsc = new wv.JSCContext();

testFeature("JSCContext - eval returns value", function() {
    return jsc.eval("2 + 2") === 4;
});

testFeature("JSCContext - state persists", function() {
    jsc.eval("var counter = 0");
    jsc.eval("counter += 10");
    jsc.eval("counter += 32");
    return jsc.eval("counter") === 42;
});

testFeature("JSCContext - set global", function() {
    jsc.set("greeting", "hello");
    return jsc.eval("greeting") === "hello";
});

testFeature("JSCContext - set object", function() {
    jsc.set("data", {x: 1, y: [2, 3]});
    return jsc.eval("data.x + data.y[1]") === 4;
});

testFeature("JSCContext - set buffer", function() {
    jsc.set("buf", new Buffer("ABC"));
    return jsc.eval("new Uint8Array(buf)[0]") === 65;
});

testFeature("JSCContext - set date", function() {
    jsc.set("d", new Date("2026-01-01T00:00:00Z"));
    return jsc.eval("d.getUTCFullYear()") === 2026;
});

testFeature("JSCContext - getGlobal", function() {
    jsc.eval("var gObj = {a: 1, b: 2}");
    var o = jsc.getGlobal("gObj");
    return o.a === 1 && o.b === 2;
});

testFeature("JSCContext - proxy method call", function() {
    jsc.eval("var obj = {add: function(a,b) { return a+b; }}");
    var obj = jsc.getGlobal("obj");
    return obj.add(3, 4) === 7;
});

testFeature("JSCContext - proxy nested access", function() {
    jsc.eval("var deep = {a: {b: {c: 99}}}");
    return jsc.getGlobal("deep").a.b.c === 99;
});

testFeature("JSCContext - proxy .toValue()", function() {
    jsc.eval("var tv = {x: [1,2,3], y: true}");
    var plain = jsc.getGlobal("tv").toValue();
    return JSON.stringify(plain) === '{"x":[1,2,3],"y":true}';
});

testFeature("JSCContext - proxy .toString()", function() {
    jsc.eval("var ts = {a: 1}");
    var s = jsc.getGlobal("ts").toString();
    return s === "[object Object]";
});

testFeature("JSCContext - pass JSC object back to JSC", function() {
    jsc.eval("function mkObj(n) { return {val: n}; }");
    jsc.eval("function rdObj(o) { return o.val * 2; }");
    var o = jsc.getGlobal("mkObj")(21);
    return jsc.getGlobal("rdObj")(o) === 42;
});

testFeature("JSCContext - Date return", function() {
    var d = jsc.eval("new Date('2026-06-15')");
    return d instanceof Date && d.getFullYear() === 2026;
});

testFeature("JSCContext - RegExp return", function() {
    var re = jsc.eval("/test/i");
    return re instanceof RegExp && re.test("TEST");
});

testFeature("JSCContext - TypedArray return", function() {
    var a = jsc.eval("new Float64Array([1.5, 2.5])");
    return a[0] === 1.5 && a[1] === 2.5;
});

testFeature("JSCContext - eval exception", function() {
    try {
        jsc.eval("throw new Error('eval boom')");
        return false;
    } catch(e) {
        return e.message.indexOf("eval boom") !== -1;
    }
});

testFeature("JSCContext - call exception", function() {
    jsc.eval("function thrower() { throw new Error('call boom'); }");
    try {
        jsc.getGlobal("thrower")();
        return false;
    } catch(e) {
        return e.message.indexOf("call boom") !== -1;
    }
});

testFeature("JSCContext - state survives exceptions", function() {
    return jsc.eval("counter") === 42;
});

testFeature("JSCContext - ES2020+ features", function() {
    jsc.eval("class Animal { #name; constructor(n){this.#name=n} get name(){return this.#name} }");
    jsc.eval("var a = new Animal('Rex')");
    return jsc.getGlobal("a").name === "Rex";
});

testFeature("JSCContext - async/await", function() {
    jsc.eval("async function aAdd(a,b) { return a+b; }");
    jsc.eval("var aResult; aAdd(3,4).then(function(v){ aResult=v; })");
    return jsc.eval("aResult") === 7;
});

testFeature("JSCContext - function-object property access", function() {
    jsc.eval("function myFunc() { return 1; }");
    jsc.eval("myFunc.extra = 42");
    var f = jsc.getGlobal("myFunc");
    return f() === 1 && f.extra === 42;
});

jsc.destroy();

testFeature("JSCContext - destroy", function() {
    try {
        jsc.eval("1+1");
        return false;
    } catch(e) {
        return true;
    }
});


/* ================================================================
   require — CommonJS shim
   ================================================================ */

testFeature("JSCContext - require returns exports", function() {
    var j = new wv.JSCContext();
    var math = j.require("libs/lodash.js");
    var ok = typeof math.VERSION === "string" && typeof math.chunk !== "undefined";
    j.destroy();
    return ok;
});


/* ================================================================
   lodash
   ================================================================ */

printf("\n");

var jsc_lo = new wv.JSCContext();
var _ = jsc_lo.require("libs/lodash.js");

testFeature("lodash - version", function() {
    return typeof _.VERSION === "string";
});

testFeature("lodash - chunk", function() {
    return JSON.stringify(_.chunk([1,2,3,4,5,6], 2).toValue()) === "[[1,2],[3,4],[5,6]]";
});

testFeature("lodash - groupBy", function() {
    return JSON.stringify(_.groupBy(["one","two","three"], "length").toValue())
        === '{"3":["one","two"],"5":["three"]}';
});

testFeature("lodash - sortBy", function() {
    return JSON.stringify(_.sortBy([3,1,4,1,5,9,2,6], _.identity).toValue())
        === "[1,1,2,3,4,5,6,9]";
});

testFeature("lodash - template", function() {
    var compiled = _.template("Hello, <%= name %>!");
    return compiled({name: "World"}) === "Hello, World!";
});

testFeature("lodash - cloneDeep", function() {
    var r = _.cloneDeep({a: {b: {c: 42}}});
    return r.a.b.c === 42;
});

testFeature("lodash - chain (via eval)", function() {
    jsc_lo.set("_", _);
    jsc_lo.set("arr", [1,2,3,4,5]);
    var r = jsc_lo.eval("_.chain(arr).filter(function(n){return n%2===0}).map(function(n){return n*10}).value()").toValue();
    return JSON.stringify(r) === "[20,40]";
});

jsc_lo.destroy();


/* ================================================================
   marked (Markdown to HTML)
   ================================================================ */

printf("\n");

var jsc_mk = new wv.JSCContext();
jsc_mk.loadScript("libs/marked.js");
var marked = jsc_mk.getGlobal("marked");

testFeature("marked - heading", function() {
    var r = marked.parse("# Hello World");
    return r.indexOf("<h1>") !== -1 && r.indexOf("Hello World") !== -1;
});

testFeature("marked - list", function() {
    var r = marked.parse("- one\n- two\n- three\n");
    return r.indexOf("<ul>") !== -1 && r.indexOf("<li>") !== -1;
});

testFeature("marked - code block", function() {
    var r = marked.parse("    var x = 1;\n");
    return r.indexOf("<code>") !== -1;
});

testFeature("marked - complex document", function() {
    var md = "# Title\n\nA paragraph with **bold** and *italic*.\n\n"
           + "| Col A | Col B |\n|---|---|\n| 1 | 2 |\n\n"
           + "> A blockquote\n";
    var r = marked.parse(md);
    return r.indexOf("<strong>bold</strong>") !== -1
        && r.indexOf("<table>") !== -1
        && r.indexOf("<blockquote>") !== -1;
});

jsc_mk.destroy();


/* ================================================================
   ajv (JSON Schema validation)
   ================================================================ */

printf("\n");

var jsc_ajv = new wv.JSCContext();
jsc_ajv.loadScript("libs/ajv.js");

testFeature("ajv - validate correct data", function() {
    return jsc_ajv.eval('\
        var ajv = new Ajv();\
        var schema = {\
            type: "object",\
            properties: {\
                name: {type: "string"},\
                age: {type: "integer", minimum: 0}\
            },\
            required: ["name", "age"]\
        };\
        var validate = ajv.compile(schema);\
        validate({name: "Alice", age: 30});\
    ') === true;
});

testFeature("ajv - reject invalid data", function() {
    return jsc_ajv.eval('validate({name: 123, age: -5})') === false;
});

testFeature("ajv - error messages", function() {
    var errors = jsc_ajv.eval("validate.errors").toValue();
    return errors.length > 0;
});

jsc_ajv.destroy();


/* ================================================================
   papaparse (CSV parsing)
   ================================================================ */

printf("\n");

var jsc_pp = new wv.JSCContext();
jsc_pp.loadScript("libs/papaparse.js");
var Papa = jsc_pp.getGlobal("Papa");

testFeature("papaparse - parse CSV", function() {
    var r = Papa.parse("name,age,city\nAlice,30,NYC\nBob,25,LA");
    var data = r.data.toValue();
    return data.length === 3
        && data[0][0] === "name"
        && data[1][0] === "Alice";
});

testFeature("papaparse - parse with header", function() {
    var r = Papa.parse("name,age\nAlice,30\nBob,25\n", {header: true});
    var data = r.data.toValue();
    return data[0].name === "Alice" && data[0].age === "30";
});

testFeature("papaparse - unparse", function() {
    jsc_pp.set("input", [{name: "Alice", age: 30}, {name: "Bob", age: 25}]);
    var csv = jsc_pp.eval("Papa.unparse(input)");
    return csv.indexOf("Alice") !== -1 && csv.indexOf("name") !== -1;
});

jsc_pp.destroy();


/* ================================================================
   handlebars (template engine)
   ================================================================ */

printf("\n");

var jsc_hb = new wv.JSCContext();
jsc_hb.loadScript("libs/handlebars.js");
var Handlebars = jsc_hb.getGlobal("Handlebars");

testFeature("handlebars - simple template", function() {
    var template = Handlebars.compile("Hello, {{name}}!");
    return template({name: "World"}) === "Hello, World!";
});

testFeature("handlebars - each helper", function() {
    var template = Handlebars.compile("{{#each items}}<li>{{this}}</li>{{/each}}");
    return template({items: ["a", "b", "c"]}) === "<li>a</li><li>b</li><li>c</li>";
});

testFeature("handlebars - conditionals", function() {
    var template = Handlebars.compile("{{#if show}}YES{{else}}NO{{/if}}");
    return template({show: true}) === "YES" && template({show: false}) === "NO";
});

testFeature("handlebars - nested context", function() {
    var template = Handlebars.compile("{{person.name}} is {{person.age}}");
    return template({person: {name: "Alice", age: 30}}) === "Alice is 30";
});

jsc_hb.destroy();


/* ================================================================
   js-yaml (YAML parsing)
   ================================================================ */

printf("\n");

var jsc_ym = new wv.JSCContext();
jsc_ym.loadScript("libs/js-yaml.js");
var jsyaml = jsc_ym.getGlobal("jsyaml");

testFeature("js-yaml - parse simple", function() {
    var r = jsyaml.load("name: Alice\nage: 30\n").toValue();
    return r.name === "Alice" && r.age === 30;
});

testFeature("js-yaml - parse nested", function() {
    var r = jsyaml.load("server:\n  host: localhost\n  port: 8080\n").toValue();
    return r.server.host === "localhost" && r.server.port === 8080;
});

testFeature("js-yaml - parse array", function() {
    var r = jsyaml.load("- one\n- two\n- three\n").toValue();
    return JSON.stringify(r) === '["one","two","three"]';
});

testFeature("js-yaml - dump", function() {
    jsc_ym.set("obj", {name: "Alice", scores: [10, 20, 30]});
    var r = jsc_ym.eval("jsyaml.dump(obj)");
    return r.indexOf("name: Alice") !== -1 && r.indexOf("- 10") !== -1;
});

jsc_ym.destroy();


/* ================================================================
   fuse.js (fuzzy search)
   ================================================================ */

printf("\n");

var jsc_fu = new wv.JSCContext();
jsc_fu.loadScript("libs/fuse.js");

testFeature("fuse.js - basic search", function() {
    jsc_fu.set("books", [
        {title: "The Great Gatsby", author: "F. Scott Fitzgerald"},
        {title: "Moby Dick", author: "Herman Melville"},
        {title: "To Kill a Mockingbird", author: "Harper Lee"},
        {title: "The Catcher in the Rye", author: "J.D. Salinger"},
        {title: "Great Expectations", author: "Charles Dickens"}
    ]);
    jsc_fu.eval("var fuse = new Fuse(books, {keys: ['title', 'author']})");
    var r = jsc_fu.eval("fuse.search('great')").toValue();
    var titles = r.map(function(x) { return x.item.title; });
    return titles.indexOf("The Great Gatsby") !== -1
        && titles.indexOf("Great Expectations") !== -1;
});

testFeature("fuse.js - fuzzy matching", function() {
    var r = jsc_fu.eval("fuse.search('melv')").toValue();
    return r.length >= 1 && r[0].item.author === "Herman Melville";
});

jsc_fu.destroy();


/* ================================================================
   validator.js (string validation)
   ================================================================ */

printf("\n");

var jsc_vl = new wv.JSCContext();
jsc_vl.loadScript("libs/validator.js");
var validator = jsc_vl.getGlobal("validator");

testFeature("validator.js - isEmail", function() {
    return validator.isEmail("test@example.com") === true
        && validator.isEmail("not-an-email") === false;
});

testFeature("validator.js - isURL", function() {
    return validator.isURL("https://example.com") === true
        && validator.isURL("not a url") === false;
});

testFeature("validator.js - isIP", function() {
    return validator.isIP("192.168.1.1") === true
        && validator.isIP("::1") === true
        && validator.isIP("999.999.999.999") === false;
});

testFeature("validator.js - isJSON", function() {
    return validator.isJSON('{"key": "value"}') === true
        && validator.isJSON('{bad json}') === false;
});

testFeature("validator.js - isUUID", function() {
    return validator.isUUID("550e8400-e29b-41d4-a716-446655440000") === true
        && validator.isUUID("not-a-uuid") === false;
});

testFeature("validator.js - escape HTML", function() {
    var escaped = validator.escape("<script>alert('xss')</script>");
    return escaped.indexOf("<") === -1 && escaped.indexOf("&lt;") !== -1;
});

jsc_vl.destroy();


/* ================================================================ */

printf("\n");
if (_nfailed)
    printf("%d test(s) FAILED\n", _nfailed);
else
    printf("All tests passed.\n");

process.exit(_nfailed ? 1 : 0);
