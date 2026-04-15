/* rampart-webview.c - Rampart module wrapping the webview library
 *
 * Copyright (C) 2026 Aaron Flin - All Rights Reserved
 * You may use, distribute or alter this code under the
 * terms of the MIT license
 * see https://opensource.org/licenses/MIT
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef WEBVIEW_STATIC
#define WEBVIEW_STATIC
#endif
#include "rampart.h"
#include "webview/api.h"
#include <jsc/jsc.h>

/* ============================================================
   Thread safety: webview requires the main thread
   ============================================================ */

#define REQUIRE_MAIN_THREAD(ctx) do { \
    if (get_thread_num() != 0) \
        RP_THROW(ctx, "webview: must be used from the main thread"); \
} while(0)

/* ============================================================
   Data structures
   ============================================================ */

/* Per-binding callback information */
typedef struct bind_info_s {
    webview_t     w;         /* the webview instance */
    duk_context  *ctx;       /* the Duktape context */
    void         *this_ptr;  /* heapptr to WebView 'this' object */
    char         *name;      /* binding name (strdup'd) */
} bind_info_t;

/* ============================================================
   Helper: extract webview_t from 'this'
   ============================================================ */

static webview_t get_wv(duk_context *ctx)
{
    webview_t w;
    duk_push_this(ctx);
    if (!duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("wv"))) {
        duk_pop_2(ctx);
        RP_THROW(ctx, "webview: instance has been destroyed or is invalid");
    }
    w = (webview_t)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    if (!w)
        RP_THROW(ctx, "webview: instance has been destroyed");
    return w;
}

/* ============================================================
   Bind callback: C bridge between webview JS and Duktape JS
   ============================================================ */

/*
 * Called from within webview_run()'s event loop on the same thread.
 * Since webview_run() blocked the Duktape thread and no other Duktape
 * code is executing, it is safe to use the same duk_context*.
 *
 * id:  sequence identifier for webview_return()
 * req: JSON array of arguments, e.g. "[3,4]"
 * arg: our bind_info_t*
 */
static void wv_bind_callback(const char *id, const char *req, void *arg)
{
    bind_info_t *bi = (bind_info_t *)arg;
    duk_context *ctx = bi->ctx;
    duk_idx_t top = duk_get_top(ctx);

    /* Push the WebView 'this' object */
    duk_push_heapptr(ctx, bi->this_ptr);

    /* Get the bindings table: this[HIDDEN("bindings")] */
    if (!duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("bindings"))) {
        duk_set_top(ctx, top);
        webview_return(bi->w, id, 1, "\"binding object not found\"");
        return;
    }

    /* Get the specific callback by name */
    if (!duk_get_prop_string(ctx, -1, bi->name) || !duk_is_function(ctx, -1)) {
        duk_set_top(ctx, top);
        webview_return(bi->w, id, 1, "\"callback not found\"");
        return;
    }
    /* stack: [this, bindings, func] */

    /* Remove bindings object and this — we just need the function */
    duk_remove(ctx, top + 1);  /* remove bindings */
    duk_remove(ctx, top);      /* remove this */
    /* stack: [func] */

    /* Parse req JSON array and push individual args */
    duk_push_string(ctx, req);
    duk_json_decode(ctx, -1);
    /* stack: [func, argsArray] */

    duk_uarridx_t nargs = (duk_uarridx_t)duk_get_length(ctx, -1);
    duk_uarridx_t i;
    for (i = 0; i < nargs; i++) {
        duk_get_prop_index(ctx, top + 1, i);
    }
    /* stack: [func, argsArray, arg0, arg1, ...] */

    /* Remove the argsArray */
    duk_remove(ctx, top + 1);
    /* stack: [func, arg0, arg1, ...] */

    /* Call the function */
    if (duk_pcall(ctx, (duk_idx_t)nargs) != 0) {
        /* Error: get message, JSON-encode it, return as error */
        const char *errmsg = duk_safe_to_string(ctx, -1);
        /* Build a JSON string for the error */
        duk_push_string(ctx, errmsg);
        duk_json_encode(ctx, -1);
        const char *json_err = duk_get_string(ctx, -1);
        webview_return(bi->w, id, 1, json_err);
        duk_set_top(ctx, top);
        return;
    }

    /* Success: JSON-encode the return value */
    if (duk_is_undefined(ctx, -1)) {
        webview_return(bi->w, id, 0, "");
    } else {
        duk_json_encode(ctx, -1);
        webview_return(bi->w, id, 0, duk_get_string(ctx, -1));
    }

    duk_set_top(ctx, top);
}

/* ============================================================
   Finalizer: clean up on GC
   ============================================================ */

static duk_ret_t wv_finalizer(duk_context *ctx)
{
    /* arg 0 is the object being finalized */
    if (!duk_get_prop_string(ctx, 0, DUK_HIDDEN_SYMBOL("wv"))) {
        duk_pop(ctx);
        return 0;
    }
    webview_t w = (webview_t)duk_get_pointer(ctx, -1);
    duk_pop(ctx);

    if (w) {
        webview_destroy(w);
        duk_push_pointer(ctx, NULL);
        duk_put_prop_string(ctx, 0, DUK_HIDDEN_SYMBOL("wv"));
    }

    /* Free bind_info_t allocations */
    if (duk_get_prop_string(ctx, 0, DUK_HIDDEN_SYMBOL("bind_infos"))) {
        duk_uarridx_t i, len = (duk_uarridx_t)duk_get_length(ctx, -1);
        for (i = 0; i < len; i++) {
            duk_get_prop_index(ctx, -1, i);
            bind_info_t *bi = (bind_info_t *)duk_get_pointer(ctx, -1);
            duk_pop(ctx);
            if (bi) {
                free(bi->name);
                free(bi);
            }
        }
    }
    duk_pop(ctx);

    return 0;
}

/* ============================================================
   Constructor: new WebView({...})
   ============================================================ */

static duk_ret_t wv_constructor(duk_context *ctx)
{
    REQUIRE_MAIN_THREAD(ctx);
    if (!duk_is_constructor_call(ctx))
        RP_THROW(ctx, "WebView must be called with 'new'");

    int debug = 0;
    const char *title = "Rampart WebView";
    int width = 800, height = 600;
    const char *url = NULL;
    const char *html = NULL;

    /* Parse options object (argument 0) */
    if (duk_is_object(ctx, 0) && !duk_is_function(ctx, 0)) {
        if (duk_get_prop_string(ctx, 0, "debug")) {
            debug = duk_to_boolean(ctx, -1);
        }
        duk_pop(ctx);

        if (duk_get_prop_string(ctx, 0, "title")) {
            title = duk_get_string(ctx, -1);
        }
        duk_pop(ctx);

        if (duk_get_prop_string(ctx, 0, "width")) {
            width = duk_get_int(ctx, -1);
        }
        duk_pop(ctx);

        if (duk_get_prop_string(ctx, 0, "height")) {
            height = duk_get_int(ctx, -1);
        }
        duk_pop(ctx);

        if (duk_get_prop_string(ctx, 0, "url")) {
            url = duk_get_string(ctx, -1);
        }
        duk_pop(ctx);

        if (duk_get_prop_string(ctx, 0, "html")) {
            html = duk_get_string(ctx, -1);
        }
        duk_pop(ctx);
    }

    /* Create webview instance */
    webview_t w = webview_create(debug, NULL);
    if (!w)
        RP_THROW(ctx, "webview: failed to create instance (missing dependencies?)");

    webview_set_title(w, title);
    webview_set_size(w, width, height, WEBVIEW_HINT_NONE);

    if (url)
        webview_navigate(w, url);
    else if (html)
        webview_set_html(w, html);

    /* Store webview pointer on 'this' */
    duk_push_this(ctx);

    duk_push_pointer(ctx, (void *)w);
    duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("wv"));

    /* Store duk_context pointer for bind callbacks */
    duk_push_pointer(ctx, (void *)ctx);
    duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("ctx"));

    /* Object to hold bound JS callbacks by name */
    duk_push_object(ctx);
    duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("bindings"));

    /* Array to hold bind_info_t pointers for cleanup */
    duk_push_array(ctx);
    duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("bind_infos"));

    /* Set finalizer for cleanup */
    duk_push_c_function(ctx, wv_finalizer, 1);
    duk_set_finalizer(ctx, -2);

    duk_pop(ctx); /* pop this */

    return 0; /* constructor returns 'this' automatically */
}

/* ============================================================
   Prototype methods
   ============================================================ */

static duk_ret_t wv_set_title(duk_context *ctx)
{
    REQUIRE_MAIN_THREAD(ctx);
    webview_t w = get_wv(ctx);
    const char *title = REQUIRE_STRING(ctx, 0,
        "webview.setTitle(): argument must be a string");
    webview_set_title(w, title);
    return 0;
}

static duk_ret_t wv_set_size(duk_context *ctx)
{
    REQUIRE_MAIN_THREAD(ctx);
    webview_t w = get_wv(ctx);
    int width = REQUIRE_INT(ctx, 0,
        "webview.setSize(): first argument (width) must be a number");
    int height = REQUIRE_INT(ctx, 1,
        "webview.setSize(): second argument (height) must be a number");
    webview_hint_t hint = WEBVIEW_HINT_NONE;

    if (duk_is_number(ctx, 2)) {
        hint = (webview_hint_t)duk_get_int(ctx, 2);
    } else if (duk_is_string(ctx, 2)) {
        const char *h = duk_get_string(ctx, 2);
        if (!strcmp(h, "none"))       hint = WEBVIEW_HINT_NONE;
        else if (!strcmp(h, "min"))   hint = WEBVIEW_HINT_MIN;
        else if (!strcmp(h, "max"))   hint = WEBVIEW_HINT_MAX;
        else if (!strcmp(h, "fixed")) hint = WEBVIEW_HINT_FIXED;
        else RP_THROW(ctx, "webview.setSize(): invalid hint '%s' "
                      "(expected 'none', 'min', 'max', or 'fixed')", h);
    }

    webview_set_size(w, width, height, hint);
    return 0;
}

static duk_ret_t wv_navigate(duk_context *ctx)
{
    REQUIRE_MAIN_THREAD(ctx);
    webview_t w = get_wv(ctx);
    const char *url = REQUIRE_STRING(ctx, 0,
        "webview.navigate(): argument must be a string (URL)");
    webview_error_t err = webview_navigate(w, url);
    if (WEBVIEW_FAILED(err))
        RP_THROW(ctx, "webview.navigate(): failed to navigate to '%s'", url);
    return 0;
}

static duk_ret_t wv_set_html(duk_context *ctx)
{
    REQUIRE_MAIN_THREAD(ctx);
    webview_t w = get_wv(ctx);
    const char *html = REQUIRE_STRING(ctx, 0,
        "webview.setHtml(): argument must be a string (HTML)");
    webview_error_t err = webview_set_html(w, html);
    if (WEBVIEW_FAILED(err))
        RP_THROW(ctx, "webview.setHtml(): failed to set HTML content");
    return 0;
}

static duk_ret_t wv_init_js(duk_context *ctx)
{
    REQUIRE_MAIN_THREAD(ctx);
    webview_t w = get_wv(ctx);
    const char *js = REQUIRE_STRING(ctx, 0,
        "webview.init(): argument must be a string (JavaScript)");
    webview_error_t err = webview_init(w, js);
    if (WEBVIEW_FAILED(err))
        RP_THROW(ctx, "webview.init(): failed to inject init script");
    return 0;
}

static duk_ret_t wv_eval(duk_context *ctx)
{
    REQUIRE_MAIN_THREAD(ctx);
    webview_t w = get_wv(ctx);
    const char *js = REQUIRE_STRING(ctx, 0,
        "webview.eval(): argument must be a string (JavaScript)");
    webview_error_t err = webview_eval(w, js);
    if (WEBVIEW_FAILED(err))
        RP_THROW(ctx, "webview.eval(): failed to evaluate JavaScript");
    return 0;
}

static duk_ret_t wv_bind(duk_context *ctx)
{
    REQUIRE_MAIN_THREAD(ctx);
    const char *name = REQUIRE_STRING(ctx, 0,
        "webview.bind(): first argument must be a string (function name)");
    REQUIRE_FUNCTION(ctx, 1,
        "webview.bind(): second argument must be a function");

    webview_t w = get_wv(ctx);

    /* Allocate bind_info */
    bind_info_t *bi = (bind_info_t *)malloc(sizeof(bind_info_t));
    if (!bi)
        RP_THROW(ctx, "webview.bind(): out of memory");
    bi->w = w;
    bi->ctx = ctx;
    bi->name = strdup(name);

    /* Get heapptr to 'this' for later callback lookup */
    duk_push_this(ctx);
    bi->this_ptr = duk_get_heapptr(ctx, -1);

    /* Store the JS callback in this[HIDDEN("bindings")][name] */
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("bindings"));
    duk_dup(ctx, 1);  /* copy the function arg */
    duk_put_prop_string(ctx, -2, name);
    duk_pop(ctx); /* pop bindings */

    /* Track bind_info_t pointer for cleanup */
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("bind_infos"));
    duk_uarridx_t len = (duk_uarridx_t)duk_get_length(ctx, -1);
    duk_push_pointer(ctx, (void *)bi);
    duk_put_prop_index(ctx, -2, len);
    duk_pop(ctx); /* pop bind_infos */

    duk_pop(ctx); /* pop this */

    /* Register with webview */
    webview_error_t err = webview_bind(w, name, wv_bind_callback, (void *)bi);
    if (WEBVIEW_FAILED(err))
        RP_THROW(ctx, "webview.bind(): failed to bind '%s'", name);

    return 0;
}

static duk_ret_t wv_unbind(duk_context *ctx)
{
    REQUIRE_MAIN_THREAD(ctx);
    const char *name = REQUIRE_STRING(ctx, 0,
        "webview.unbind(): argument must be a string (function name)");
    webview_t w = get_wv(ctx);

    webview_error_t err = webview_unbind(w, name);
    if (WEBVIEW_FAILED(err))
        RP_THROW(ctx, "webview.unbind(): binding '%s' not found", name);

    /* Remove from bindings object */
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("bindings"));
    duk_del_prop_string(ctx, -1, name);
    duk_pop_2(ctx);

    return 0;
}

static duk_ret_t wv_run(duk_context *ctx)
{
    REQUIRE_MAIN_THREAD(ctx);
    webview_t w = get_wv(ctx);
    webview_error_t err = webview_run(w);
    if (WEBVIEW_FAILED(err))
        RP_THROW(ctx, "webview.run(): event loop error");
    return 0;
}

static duk_ret_t wv_terminate(duk_context *ctx)
{
    REQUIRE_MAIN_THREAD(ctx);
    webview_t w = get_wv(ctx);
    webview_terminate(w);
    return 0;
}

static duk_ret_t wv_destroy(duk_context *ctx)
{
    REQUIRE_MAIN_THREAD(ctx);
    duk_push_this(ctx);

    if (!duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("wv"))) {
        duk_pop_2(ctx);
        return 0; /* already destroyed */
    }
    webview_t w = (webview_t)duk_get_pointer(ctx, -1);
    duk_pop(ctx);

    if (w) {
        webview_destroy(w);
    }

    /* Null out the pointer to prevent double-free from finalizer */
    duk_push_pointer(ctx, NULL);
    duk_put_prop_string(ctx, -2, DUK_HIDDEN_SYMBOL("wv"));

    /* Free all bind_info_t allocations */
    if (duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("bind_infos"))) {
        duk_uarridx_t i, len = (duk_uarridx_t)duk_get_length(ctx, -1);
        for (i = 0; i < len; i++) {
            duk_get_prop_index(ctx, -1, i);
            bind_info_t *bi = (bind_info_t *)duk_get_pointer(ctx, -1);
            duk_pop(ctx);
            if (bi) {
                free(bi->name);
                free(bi);
            }
        }
        /* Clear the array */
        duk_push_array(ctx);
        duk_put_prop_string(ctx, -3, DUK_HIDDEN_SYMBOL("bind_infos"));
    }
    duk_pop(ctx); /* pop bind_infos or undefined */
    duk_pop(ctx); /* pop this */

    return 0;
}

/* ============================================================
   Headless JavaScriptCore execution
   ============================================================ */

/* Maximum recursion depth for JSC → Duktape value conversion */
#define JSC_MAX_DEPTH 64

/*
 * Recursively convert a JSCValue to its Duktape equivalent,
 * pushing the result onto the Duktape stack.
 *
 * Rich type mapping (beyond what JSON can represent):
 *   JSC undefined    → duk undefined
 *   JSC null         → duk null
 *   JSC boolean      → duk boolean
 *   JSC number       → duk number (including NaN, Infinity)
 *   JSC string       → duk string
 *   JSC Date         → duk Date (preserving millisecond timestamp)
 *   JSC ArrayBuffer  → duk Node.js Buffer
 *   JSC TypedArray   → duk TypedArray (matching element type)
 *   JSC RegExp       → duk RegExp (with source and flags)
 *   JSC Error        → duk Error (with name and message)
 *   JSC Array        → duk Array (recursive)
 *   JSC Map          → duk Array of [key, value] pairs
 *   JSC Set          → duk Array of values
 *   JSC Object       → duk Object (own enumerable properties, recursive)
 *   JSC Function     → duk string (toString representation)
 *   anything else    → duk string (toString fallback)
 */
static void jsc_to_duk(duk_context *ctx, JSCContext *jsc_ctx,
                        JSCValue *value, int depth)
{
    if (depth > JSC_MAX_DEPTH)
        RP_THROW(ctx, "jscExec: object nesting too deep (>%d levels)",
                 JSC_MAX_DEPTH);

    /* --- Primitives --- */

    if (jsc_value_is_undefined(value)) {
        duk_push_undefined(ctx);
        return;
    }
    if (jsc_value_is_null(value)) {
        duk_push_null(ctx);
        return;
    }
    if (jsc_value_is_boolean(value)) {
        duk_push_boolean(ctx, jsc_value_to_boolean(value));
        return;
    }
    if (jsc_value_is_number(value)) {
        duk_push_number(ctx, jsc_value_to_double(value));
        return;
    }
    if (jsc_value_is_string(value)) {
        char *str = jsc_value_to_string(value);
        duk_push_string(ctx, str);
        g_free(str);
        return;
    }

    /* --- TypedArray (check before ArrayBuffer — typed arrays have one) --- */

    if (jsc_value_is_typed_array(value)) {
        gsize elem_len;
        gpointer data = jsc_value_typed_array_get_data(value, &elem_len);
        gsize byte_size = jsc_value_typed_array_get_size(value);
        JSCTypedArrayType jsc_type = jsc_value_typed_array_get_type(value);

        void *buf = duk_push_fixed_buffer(ctx, byte_size);
        if (data && byte_size > 0)
            memcpy(buf, data, byte_size);

        duk_uint_t duk_buftype;
        switch (jsc_type) {
            case JSC_TYPED_ARRAY_INT8:          duk_buftype = DUK_BUFOBJ_INT8ARRAY; break;
            case JSC_TYPED_ARRAY_UINT8:         duk_buftype = DUK_BUFOBJ_UINT8ARRAY; break;
            case JSC_TYPED_ARRAY_UINT8_CLAMPED: duk_buftype = DUK_BUFOBJ_UINT8CLAMPEDARRAY; break;
            case JSC_TYPED_ARRAY_INT16:         duk_buftype = DUK_BUFOBJ_INT16ARRAY; break;
            case JSC_TYPED_ARRAY_UINT16:        duk_buftype = DUK_BUFOBJ_UINT16ARRAY; break;
            case JSC_TYPED_ARRAY_INT32:         duk_buftype = DUK_BUFOBJ_INT32ARRAY; break;
            case JSC_TYPED_ARRAY_UINT32:        duk_buftype = DUK_BUFOBJ_UINT32ARRAY; break;
            case JSC_TYPED_ARRAY_FLOAT32:       duk_buftype = DUK_BUFOBJ_FLOAT32ARRAY; break;
            case JSC_TYPED_ARRAY_FLOAT64:       duk_buftype = DUK_BUFOBJ_FLOAT64ARRAY; break;
            default:
                /* INT64/UINT64 have no Duktape equivalent — use raw bytes */
                duk_buftype = DUK_BUFOBJ_UINT8ARRAY;
                break;
        }

        duk_push_buffer_object(ctx, -1, 0, byte_size, duk_buftype);
        duk_remove(ctx, -2); /* remove plain buffer, keep typed-array object */
        return;
    }

    /* --- ArrayBuffer --- */

    if (jsc_value_is_array_buffer(value)) {
        gsize size;
        gpointer data = jsc_value_array_buffer_get_data(value, &size);

        void *buf = duk_push_fixed_buffer(ctx, size);
        if (data && size > 0)
            memcpy(buf, data, size);

        duk_push_buffer_object(ctx, -1, 0, size, DUK_BUFOBJ_NODEJS_BUFFER);
        duk_remove(ctx, -2);
        return;
    }

    /* --- From here on, everything is an object of some kind --- */

    if (!jsc_value_is_object(value)) {
        /* Unknown primitive (Symbol, BigInt, etc.) — stringify */
        char *str = jsc_value_to_string(value);
        duk_push_string(ctx, str);
        g_free(str);
        return;
    }

    /* --- Date --- */

    if (jsc_value_object_is_instance_of(value, "Date")) {
        JSCValue *ts = jsc_value_object_invoke_method(value, "getTime",
                                                       G_TYPE_NONE);
        double ms = jsc_value_to_double(ts);
        g_object_unref(ts);

        duk_get_global_string(ctx, "Date");
        duk_push_number(ctx, ms);
        duk_new(ctx, 1);
        return;
    }

    /* --- RegExp --- */

    if (jsc_value_object_is_instance_of(value, "RegExp")) {
        JSCValue *source = jsc_value_object_get_property(value, "source");
        JSCValue *flags  = jsc_value_object_get_property(value, "flags");
        char *src_str   = jsc_value_to_string(source);
        char *flags_str = jsc_value_to_string(flags);

        duk_get_global_string(ctx, "RegExp");
        duk_push_string(ctx, src_str);
        duk_push_string(ctx, flags_str);
        duk_new(ctx, 2);

        g_free(src_str);
        g_free(flags_str);
        g_object_unref(source);
        g_object_unref(flags);
        return;
    }

    /* --- Error (and subclasses: TypeError, RangeError, etc.) --- */

    if (jsc_value_object_is_instance_of(value, "Error")) {
        JSCValue *msg_val  = jsc_value_object_get_property(value, "message");
        JSCValue *name_val = jsc_value_object_get_property(value, "name");
        char *msg  = jsc_value_to_string(msg_val);
        char *name = jsc_value_to_string(name_val);

        duk_push_error_object(ctx, DUK_ERR_ERROR, "%s", msg);
        duk_push_string(ctx, name);
        duk_put_prop_string(ctx, -2, "name");

        g_free(msg);
        g_free(name);
        g_object_unref(msg_val);
        g_object_unref(name_val);
        return;
    }

    /* --- Map → Array of [key, value] pairs --- */

    if (jsc_value_object_is_instance_of(value, "Map")) {
        jsc_context_set_value(jsc_ctx, "__rp_cvt", value);
        JSCValue *arr = jsc_context_evaluate(jsc_ctx,
            "Array.from(__rp_cvt.entries())", -1);
        JSCValue *tmp = jsc_context_evaluate(jsc_ctx,
            "delete globalThis.__rp_cvt", -1);
        jsc_to_duk(ctx, jsc_ctx, arr, depth + 1);
        g_object_unref(arr);
        g_object_unref(tmp);
        return;
    }

    /* --- Set → Array --- */

    if (jsc_value_object_is_instance_of(value, "Set")) {
        jsc_context_set_value(jsc_ctx, "__rp_cvt", value);
        JSCValue *arr = jsc_context_evaluate(jsc_ctx,
            "Array.from(__rp_cvt)", -1);
        JSCValue *tmp = jsc_context_evaluate(jsc_ctx,
            "delete globalThis.__rp_cvt", -1);
        jsc_to_duk(ctx, jsc_ctx, arr, depth + 1);
        g_object_unref(arr);
        g_object_unref(tmp);
        return;
    }

    /* --- Array --- */

    if (jsc_value_is_array(value)) {
        JSCValue *len_val = jsc_value_object_get_property(value, "length");
        int len = jsc_value_to_int32(len_val);
        g_object_unref(len_val);

        duk_push_array(ctx);
        for (int i = 0; i < len; i++) {
            JSCValue *elem = jsc_value_object_get_property_at_index(
                value, (guint)i);
            jsc_to_duk(ctx, jsc_ctx, elem, depth + 1);
            g_object_unref(elem);
            duk_put_prop_index(ctx, -2, (duk_uarridx_t)i);
        }
        return;
    }

    /* --- Function → source text --- */

    if (jsc_value_is_function(value)) {
        char *str = jsc_value_to_string(value);
        duk_push_string(ctx, str);
        g_free(str);
        return;
    }

    /* --- Generic object (own enumerable properties) --- */

    duk_push_object(ctx);
    gchar **props = jsc_value_object_enumerate_properties(value);
    if (props) {
        for (int i = 0; props[i]; i++) {
            JSCValue *prop = jsc_value_object_get_property(value, props[i]);
            jsc_to_duk(ctx, jsc_ctx, prop, depth + 1);
            g_object_unref(prop);
            duk_put_prop_string(ctx, -2, props[i]);
        }
        g_strfreev(props);
    }
}

/* Exception handler that captures the exception (the default handler
   just logs to stderr, and in both cases the context clears the
   exception after calling the handler — so we must grab it here). */
typedef struct {
    JSCException *exception;  /* NULL or a ref we own */
} jsc_error_capture_t;

static void jsc_capture_exception(JSCContext *context,
                                   JSCException *exception,
                                   gpointer user_data)
{
    jsc_error_capture_t *cap = (jsc_error_capture_t *)user_data;
    (void)context;
    if (cap->exception)
        g_object_unref(cap->exception);
    cap->exception = (JSCException *)g_object_ref(exception);
}

/* duk_safe_call wrapper so JSC objects are freed even if conversion throws */
typedef struct {
    JSCContext *jsc_ctx;
    JSCValue   *result;
} jsc_convert_args_t;

static duk_ret_t jsc_convert_safe(duk_context *ctx, void *udata)
{
    jsc_convert_args_t *a = (jsc_convert_args_t *)udata;
    jsc_to_duk(ctx, a->jsc_ctx, a->result, 0);
    return 1;
}

/*
 * webview.jscExec(code) → value
 *
 * Create a headless JavaScriptCore context, evaluate the given
 * JavaScript code, and return the result with rich type conversion
 * (Dates, Buffers, TypedArrays, RegExps, etc. — not just JSON).
 */
static duk_ret_t jsc_exec(duk_context *ctx)
{
    const char *js = REQUIRE_STRING(ctx, 0,
        "jscExec(): argument must be a string (JavaScript code)");

    JSCContext *jsc_ctx = jsc_context_new();
    if (!jsc_ctx)
        RP_THROW(ctx, "jscExec(): failed to create JavaScriptCore context");

    /* Install a handler that captures the exception object (the
       context clears it after calling the handler, so get_exception()
       alone is not reliable). */
    jsc_error_capture_t err_cap = { NULL };
    jsc_context_push_exception_handler(jsc_ctx,
        jsc_capture_exception, &err_cap, NULL);

    JSCValue *result = jsc_context_evaluate(jsc_ctx, js, -1);

    /* Check for a JSC exception (syntax error, runtime error, etc.) */
    if (err_cap.exception) {
        char *report = jsc_exception_report(err_cap.exception);
        g_object_unref(err_cap.exception);
        if (result)
            g_object_unref(result);
        g_object_unref(jsc_ctx);
        /* Push error, free C string, then throw */
        duk_push_error_object(ctx, DUK_ERR_ERROR, "jscExec: %s", report);
        g_free(report);
        (void)duk_throw(ctx);
        return 0; /* unreachable */
    }

    /* Convert JSC value → Duktape value inside a safe call so that
       any Duktape throw during conversion doesn't leak JSC objects. */
    jsc_convert_args_t args = { jsc_ctx, result };
    duk_int_t rc = duk_safe_call(ctx, jsc_convert_safe, &args, 0, 1);

    g_object_unref(result);
    g_object_unref(jsc_ctx);

    if (rc != 0) {
        /* Re-throw the conversion error (already on the stack) */
        (void)duk_throw(ctx);
        return 0; /* unreachable */
    }

    return 1; /* converted value is on the Duktape stack */
}

/* ============================================================
   Persistent JSCContext: load JS modules and call methods
   ============================================================ */

/* --- Ref-counted persistent context --- */

typedef struct jsc_pctx_s {
    JSCContext   *ctx;        /* the JavaScriptCore context            */
    JSCException *exception;  /* last captured exception (or NULL)     */
    int           refcount;   /* prevented from being freed while      */
    pid_t         pid;        /* process that created this context     */
    int           thread_num; /* thread that created this context      */
} jsc_pctx_t;                /* (JSC is not thread-safe or fork-safe) */

static void jsc_pctx_exc_handler(JSCContext *context,
                                  JSCException *exception,
                                  gpointer user_data)
{
    jsc_pctx_t *p = (jsc_pctx_t *)user_data;
    (void)context;
    if (p->exception)
        g_object_unref(p->exception);
    p->exception = (JSCException *)g_object_ref(exception);
}

static jsc_pctx_t *jsc_pctx_new(void)
{
    jsc_pctx_t *p = (jsc_pctx_t *)calloc(1, sizeof(jsc_pctx_t));
    if (!p) return NULL;
    p->ctx = jsc_context_new();
    if (!p->ctx) { free(p); return NULL; }
    p->refcount = 1;
    p->pid = getpid();
    p->thread_num = get_thread_num();
    jsc_context_push_exception_handler(p->ctx,
        jsc_pctx_exc_handler, p, NULL);
    return p;
}

/* Verify we're in the same process AND thread that created the context.
   JSC contexts are single-threaded and use background threads (JIT, GC)
   that are not fork-safe. */
static void jsc_check_pid(duk_context *ctx, jsc_pctx_t *p)
{
    if (p->pid != getpid())
        RP_THROW(ctx, "JSCContext: cannot be used after fork() or daemon() "
                 "(created in pid %d, current pid %d)",
                 (int)p->pid, (int)getpid());
    if (p->thread_num != get_thread_num())
        RP_THROW(ctx, "JSCContext: cannot be used from a different thread "
                 "(created in thread %d, current thread %d)",
                 p->thread_num, get_thread_num());
}

static jsc_pctx_t *jsc_pctx_ref(jsc_pctx_t *p)  { p->refcount++; return p; }

static void jsc_pctx_unref(jsc_pctx_t *p)
{
    if (--p->refcount <= 0) {
        if (p->exception) g_object_unref(p->exception);
        if (p->ctx) g_object_unref(p->ctx);
        free(p);
    }
}

static void jsc_pctx_clear(jsc_pctx_t *p)
{
    if (p->exception) { g_object_unref(p->exception); p->exception = NULL; }
}

/* If an exception was captured, throw it as a Duktape error. */
static void jsc_check_and_throw(duk_context *ctx, jsc_pctx_t *p)
{
    if (!p->exception) return;
    char *report = jsc_exception_report(p->exception);
    g_object_unref(p->exception);
    p->exception = NULL;
    duk_push_error_object(ctx, DUK_ERR_ERROR, "%s", report);
    g_free(report);
    (void)duk_throw(ctx);
}

/* --- Hidden-symbol names for wrapped values --- */

#define JSCVAL_SYM      DUK_HIDDEN_SYMBOL("jscval")
#define JSCPCTX_SYM     DUK_HIDDEN_SYMBOL("jsc_pctx")
#define JSCPARENT_SYM   DUK_HIDDEN_SYMBOL("jsc_parent")
#define JSCMETHOD_SYM   DUK_HIDDEN_SYMBOL("jsc_method")

/* Regular (non-hidden) key for detecting wrapped JSCValues through
   Proxy objects.  Hidden symbols bypass proxy traps in Duktape,
   so duk_to_jsc() uses this instead for unwrapping. */
#define JSC_REF_KEY     "__jsc__"

/* Forward declarations (mutual recursion between wrap/convert/proxy) */
static void     jsc_wrap_value(duk_context *ctx, jsc_pctx_t *pctx,
                               JSCValue *value);
static JSCValue *duk_to_jsc(duk_context *ctx, duk_idx_t idx,
                             jsc_pctx_t *pctx);
static duk_ret_t jsc_proxy_get(duk_context *ctx);
static duk_ret_t jsc_func_call(duk_context *ctx);

/* --- Finalizer for any Duktape object wrapping a JSCValue --- */

static duk_ret_t jscval_finalizer(duk_context *ctx)
{
    /* arg 0 = object being finalized */
    if (duk_get_prop_string(ctx, 0, JSCVAL_SYM)) {
        JSCValue *v = (JSCValue *)duk_get_pointer(ctx, -1);
        if (v) g_object_unref(v);
    }
    duk_pop(ctx);
    if (duk_get_prop_string(ctx, 0, JSCPARENT_SYM)) {
        JSCValue *v = (JSCValue *)duk_get_pointer(ctx, -1);
        if (v) g_object_unref(v);
    }
    duk_pop(ctx);
    if (duk_get_prop_string(ctx, 0, JSCPCTX_SYM)) {
        jsc_pctx_t *p = (jsc_pctx_t *)duk_get_pointer(ctx, -1);
        if (p) jsc_pctx_unref(p);
    }
    duk_pop(ctx);
    return 0;
}

/* Helper: get pctx pointer from object at idx */
static jsc_pctx_t *get_jscval_pctx(duk_context *ctx, duk_idx_t idx)
{
    jsc_pctx_t *p = NULL;
    if (duk_get_prop_string(ctx, idx, JSCPCTX_SYM))
        p = (jsc_pctx_t *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);
    return p;
}

/* --- .toValue(): deep-convert the wrapped JSCValue to a Duktape value --- */

static duk_ret_t jsc_val_toValue(duk_context *ctx)
{
    duk_push_this(ctx);
    if (!duk_get_prop_string(ctx, -1, JSCVAL_SYM)) {
        duk_pop_2(ctx); duk_push_undefined(ctx); return 1;
    }
    JSCValue *val = (JSCValue *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);
    jsc_pctx_t *pctx = get_jscval_pctx(ctx, -1);
    duk_pop(ctx); /* this */
    if (!val || !pctx) { duk_push_undefined(ctx); return 1; }
    jsc_to_duk(ctx, pctx->ctx, val, 0);
    return 1;
}

/* --- .toString() --- */

static duk_ret_t jsc_val_toString(duk_context *ctx)
{
    duk_push_this(ctx);
    if (!duk_get_prop_string(ctx, -1, JSCVAL_SYM)) {
        duk_pop_2(ctx);
        duk_push_string(ctx, "[JSCValue destroyed]");
        return 1;
    }
    JSCValue *val = (JSCValue *)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    if (!val) { duk_push_string(ctx, "[JSCValue destroyed]"); return 1; }
    char *s = jsc_value_to_string(val);
    duk_push_string(ctx, s);
    g_free(s);
    return 1;
}

/* --- Store common hidden props + finalizer on the Duktape object at idx --- */

static void jsc_attach_value(duk_context *ctx, duk_idx_t idx,
                              JSCValue *val, jsc_pctx_t *pctx)
{
    idx = duk_normalize_index(ctx, idx);
    duk_push_pointer(ctx, g_object_ref(val));
    duk_put_prop_string(ctx, idx, JSCVAL_SYM);
    duk_push_pointer(ctx, jsc_pctx_ref(pctx));
    duk_put_prop_string(ctx, idx, JSCPCTX_SYM);
    /* Non-hidden ref for detection through proxies (see JSC_REF_KEY) */
    duk_push_pointer(ctx, val);  /* same ptr, lifetime held by JSCVAL_SYM */
    duk_put_prop_string(ctx, idx, JSC_REF_KEY);
    duk_push_c_function(ctx, jsc_val_toValue, 0);
    duk_put_prop_string(ctx, idx, "toValue");
    duk_push_c_function(ctx, jsc_val_toString, 0);
    duk_put_prop_string(ctx, idx, "toString");
    duk_push_c_function(ctx, jsc_val_toValue, 0);
    duk_put_prop_string(ctx, idx, "valueOf");
    duk_push_c_function(ctx, jscval_finalizer, 1);
    duk_set_finalizer(ctx, idx);
}

/* --- Duktape → JSCValue conversion --- */

static JSCValue *duk_to_jsc(duk_context *ctx, duk_idx_t idx,
                             jsc_pctx_t *pctx)
{
    JSCContext *jc = pctx->ctx;
    idx = duk_normalize_index(ctx, idx);

    /* Unwrap proxy-wrapped or function-wrapped JSCValues.
       Uses JSC_REF_KEY (non-hidden) so the lookup goes through
       proxy get traps, which forward to the target. */
    if (duk_is_object(ctx, idx)) {
        if (duk_get_prop_string(ctx, idx, JSC_REF_KEY)) {
            JSCValue *v = (JSCValue *)duk_get_pointer(ctx, -1);
            duk_pop(ctx);
            if (v) return (JSCValue *)g_object_ref(v);
        } else {
            duk_pop(ctx);
        }
    }

    switch (duk_get_type(ctx, idx)) {
    case DUK_TYPE_UNDEFINED:
        return jsc_value_new_undefined(jc);
    case DUK_TYPE_NULL:
        return jsc_value_new_null(jc);
    case DUK_TYPE_BOOLEAN:
        return jsc_value_new_boolean(jc, duk_get_boolean(ctx, idx));
    case DUK_TYPE_NUMBER:
        return jsc_value_new_number(jc, duk_get_number(ctx, idx));
    case DUK_TYPE_STRING:
        return jsc_value_new_string(jc, duk_get_string(ctx, idx));
    case DUK_TYPE_BUFFER: {
        duk_size_t sz;
        void *data = duk_get_buffer_data(ctx, idx, &sz);
        void *copy = g_malloc(sz > 0 ? sz : 1);
        if (data && sz > 0) memcpy(copy, data, sz);
        return jsc_value_new_array_buffer(jc, copy, sz, g_free, copy);
    }
    case DUK_TYPE_OBJECT: {
        /* Duktape functions (unwrapped) → stringify */
        if (duk_is_function(ctx, idx)) {
            duk_dup(ctx, idx);
            const char *s = duk_safe_to_string(ctx, -1);
            JSCValue *v = jsc_value_new_string(jc, s);
            duk_pop(ctx);
            return v;
        }

        /* Buffer data (TypedArrays, Node.js Buffers) */
        if (duk_is_buffer_data(ctx, idx)) {
            duk_size_t sz;
            void *data = duk_get_buffer_data(ctx, idx, &sz);
            void *copy = g_malloc(sz > 0 ? sz : 1);
            if (data && sz > 0) memcpy(copy, data, sz);
            return jsc_value_new_array_buffer(jc, copy, sz, g_free, copy);
        }

        /* Date → new Date(ms) */
        duk_get_global_string(ctx, "Date");
        int is_date = duk_instanceof(ctx, idx, -1);
        duk_pop(ctx);
        if (is_date) {
            duk_dup(ctx, idx);
            duk_get_prop_string(ctx, -1, "getTime");
            duk_dup(ctx, -2);
            duk_call_method(ctx, 0);
            double ms = duk_get_number(ctx, -1);
            duk_pop_2(ctx);
            char buf[64];
            snprintf(buf, sizeof(buf), "new Date(%.17g)", ms);
            return jsc_context_evaluate(jc, buf, -1);
        }

        /* Array */
        if (duk_is_array(ctx, idx)) {
            duk_size_t len = duk_get_length(ctx, idx);
            JSCValue *arr = jsc_context_evaluate(jc, "[]", -1);
            for (duk_uarridx_t i = 0; i < (duk_uarridx_t)len; i++) {
                duk_get_prop_index(ctx, idx, i);
                JSCValue *elem = duk_to_jsc(ctx, -1, pctx);
                duk_pop(ctx);
                jsc_value_object_set_property_at_index(arr, i, elem);
                g_object_unref(elem);
            }
            return arr;
        }

        /* Plain object */
        {
            JSCValue *obj = jsc_context_evaluate(jc, "({})", -1);
            duk_enum(ctx, idx, DUK_ENUM_OWN_PROPERTIES_ONLY);
            while (duk_next(ctx, -1, 1)) {
                const char *k = duk_to_string(ctx, -2);
                JSCValue *v = duk_to_jsc(ctx, -1, pctx);
                jsc_value_object_set_property(obj, k, v);
                g_object_unref(v);
                duk_pop_2(ctx);
            }
            duk_pop(ctx); /* enum */
            return obj;
        }
    }
    default:
        return jsc_value_new_undefined(jc);
    }
}

/* --- C wrapper called when a proxied JSC function is invoked --- */

static duk_ret_t jsc_func_call(duk_context *ctx)
{
    duk_idx_t nargs = duk_get_top(ctx);

    duk_push_current_function(ctx);
    duk_idx_t fn = duk_get_top_index(ctx);

    if (!duk_get_prop_string(ctx, fn, JSCVAL_SYM))
        RP_THROW(ctx, "JSCContext: function reference is invalid");
    JSCValue *func = (JSCValue *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);

    jsc_pctx_t *pctx = get_jscval_pctx(ctx, fn);
    if (!pctx || !pctx->ctx)
        RP_THROW(ctx, "JSCContext: context has been destroyed");
    jsc_check_pid(ctx, pctx);

    /* Parent object + method name (for correct 'this' binding) */
    JSCValue *parent = NULL;
    const char *method = NULL;
    if (duk_get_prop_string(ctx, fn, JSCPARENT_SYM))
        parent = (JSCValue *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);
    if (parent) {
        if (duk_get_prop_string(ctx, fn, JSCMETHOD_SYM))
            method = duk_get_string(ctx, -1);
        duk_pop(ctx);
    }

    duk_pop(ctx); /* current_function */

    /* Convert arguments Duktape → JSC */
    JSCValue **ja = NULL;
    if (nargs > 0) {
        ja = (JSCValue **)malloc(sizeof(JSCValue *) * (size_t)nargs);
        if (!ja) RP_THROW(ctx, "JSCContext: out of memory");
        for (duk_idx_t i = 0; i < nargs; i++)
            ja[i] = duk_to_jsc(ctx, i, pctx);
    }

    /* Call */
    jsc_pctx_clear(pctx);
    JSCValue *result;
    if (parent && method)
        result = jsc_value_object_invoke_methodv(parent, method,
                     (guint)nargs, ja);
    else
        result = jsc_value_function_callv(func, (guint)nargs, ja);

    /* Free converted args */
    for (duk_idx_t i = 0; i < nargs; i++)
        g_object_unref(ja[i]);
    free(ja);

    /* Check exception (args already freed, safe to throw) */
    if (pctx->exception) {
        if (result) g_object_unref(result);
        jsc_check_and_throw(ctx, pctx);
        return 0;
    }

    jsc_wrap_value(ctx, pctx, result);
    g_object_unref(result);
    return 1;
}

/* --- Proxy get trap: resolve JSC object properties lazily --- */

static duk_ret_t jsc_proxy_get(duk_context *ctx)
{
    /* args: 0=target, 1=property, 2=receiver */

    /* Pass through properties that already exist on the target
       (hidden symbols, toValue, toString, valueOf, etc.) */
    duk_dup(ctx, 1);
    if (duk_get_prop(ctx, 0))
        return 1;
    duk_pop(ctx); /* undefined */

    const char *key = duk_get_string(ctx, 1);
    if (!key) return 0;

    /* Get the wrapped JSCValue from the target */
    if (!duk_get_prop_string(ctx, 0, JSCVAL_SYM)) { duk_pop(ctx); return 0; }
    JSCValue *obj = (JSCValue *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);
    if (!obj) return 0;

    jsc_pctx_t *pctx = get_jscval_pctx(ctx, 0);
    if (!pctx || !pctx->ctx) return 0;
    jsc_check_pid(ctx, pctx);

    /* Look up the property in JSC */
    if (!jsc_value_object_has_property(obj, key))
        return 0;

    JSCValue *val = jsc_value_object_get_property(obj, key);
    if (!val) return 0;

    /* Functions → callable wrapper with parent/method for correct 'this' */
    if (jsc_value_is_function(val)) {
        duk_push_c_function(ctx, jsc_func_call, DUK_VARARGS);
        /* val ref is transferred (we own it from get_property) */
        duk_push_pointer(ctx, val);
        duk_put_prop_string(ctx, -2, JSCVAL_SYM);
        duk_push_pointer(ctx, val);  /* non-hidden ref for duk_to_jsc */
        duk_put_prop_string(ctx, -2, JSC_REF_KEY);
        duk_push_pointer(ctx, (JSCValue *)g_object_ref(obj));
        duk_put_prop_string(ctx, -2, JSCPARENT_SYM);
        duk_push_string(ctx, key);
        duk_put_prop_string(ctx, -2, JSCMETHOD_SYM);
        duk_push_pointer(ctx, jsc_pctx_ref(pctx));
        duk_put_prop_string(ctx, -2, JSCPCTX_SYM);
        duk_push_c_function(ctx, jsc_val_toValue, 0);
        duk_put_prop_string(ctx, -2, "toValue");
        duk_push_c_function(ctx, jsc_val_toString, 0);
        duk_put_prop_string(ctx, -2, "toString");
        duk_push_c_function(ctx, jscval_finalizer, 1);
        duk_set_finalizer(ctx, -2);
        return 1;
    }

    /* Objects → proxy wrapper */
    if (jsc_value_is_object(val)) {
        jsc_wrap_value(ctx, pctx, val);
        g_object_unref(val);
        return 1;
    }

    /* Primitives → direct conversion */
    jsc_to_duk(ctx, pctx->ctx, val, 0);
    g_object_unref(val);
    return 1;
}

/* --- Proxy has trap --- */

static duk_ret_t jsc_proxy_has(duk_context *ctx)
{
    /* args: 0=target, 1=property */
    duk_dup(ctx, 1);
    if (duk_has_prop(ctx, 0)) { duk_push_true(ctx); return 1; }

    const char *key = duk_get_string(ctx, 1);
    if (!key) { duk_push_false(ctx); return 1; }

    if (!duk_get_prop_string(ctx, 0, JSCVAL_SYM)) {
        duk_pop(ctx); duk_push_false(ctx); return 1;
    }
    JSCValue *obj = (JSCValue *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);
    duk_push_boolean(ctx, obj && jsc_value_object_has_property(obj, key));
    return 1;
}

/* --- Proxy ownKeys trap --- */

static duk_ret_t jsc_proxy_ownKeys(duk_context *ctx)
{
    /* arg 0=target */
    duk_push_array(ctx);

    if (!duk_get_prop_string(ctx, 0, JSCVAL_SYM)) { duk_pop(ctx); return 1; }
    JSCValue *obj = (JSCValue *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);
    if (!obj) return 1;

    gchar **props = jsc_value_object_enumerate_properties(obj);
    if (props) {
        for (duk_uarridx_t i = 0; props[i]; i++) {
            duk_push_string(ctx, props[i]);
            duk_put_prop_index(ctx, -2, i);
        }
        g_strfreev(props);
    }
    return 1;
}

/* --- Proxy getOwnPropertyDescriptor trap (needed for ownKeys to work) --- */

static duk_ret_t jsc_proxy_getOwnPropDesc(duk_context *ctx)
{
    /* args: 0=target, 1=property */
    const char *key = duk_get_string(ctx, 1);
    if (!key) return 0;

    /* Check target */
    duk_dup(ctx, 1);
    if (duk_get_prop(ctx, 0)) {
        duk_push_object(ctx);
        duk_pull(ctx, -2);
        duk_put_prop_string(ctx, -2, "value");
        duk_push_true(ctx); duk_put_prop_string(ctx, -2, "writable");
        duk_push_true(ctx); duk_put_prop_string(ctx, -2, "enumerable");
        duk_push_true(ctx); duk_put_prop_string(ctx, -2, "configurable");
        return 1;
    }
    duk_pop(ctx);

    /* Check JSC object */
    if (!duk_get_prop_string(ctx, 0, JSCVAL_SYM)) { duk_pop(ctx); return 0; }
    JSCValue *obj = (JSCValue *)duk_get_pointer(ctx, -1);
    duk_pop(ctx);
    if (!obj || !jsc_value_object_has_property(obj, key))
        return 0;

    /* Report it as configurable+enumerable (value resolved lazily via get) */
    duk_push_object(ctx);
    duk_push_true(ctx); duk_put_prop_string(ctx, -2, "writable");
    duk_push_true(ctx); duk_put_prop_string(ctx, -2, "enumerable");
    duk_push_true(ctx); duk_put_prop_string(ctx, -2, "configurable");

    /* Provide the value now (avoids invariant violations) */
    JSCValue *val = jsc_value_object_get_property(obj, key);
    if (val) {
        jsc_pctx_t *pctx = get_jscval_pctx(ctx, 0);
        if (pctx) {
            jsc_wrap_value(ctx, pctx, val);
            duk_put_prop_string(ctx, -2, "value");
        }
        g_object_unref(val);
    }
    return 1;
}

/* --- Wrap a JSCValue: primitives convert, objects get proxied --- */

static void jsc_wrap_value(duk_context *ctx, jsc_pctx_t *pctx,
                            JSCValue *value)
{
    /* Primitives: direct conversion */
    if (jsc_value_is_undefined(value) || jsc_value_is_null(value) ||
        jsc_value_is_boolean(value)   || jsc_value_is_number(value) ||
        jsc_value_is_string(value)) {
        jsc_to_duk(ctx, pctx->ctx, value, 0);
        return;
    }

    /* Binary data: always copy to Duktape buffers */
    if (jsc_value_is_typed_array(value) || jsc_value_is_array_buffer(value)) {
        jsc_to_duk(ctx, pctx->ctx, value, 0);
        return;
    }

    if (!jsc_value_is_object(value)) {
        char *s = jsc_value_to_string(value);
        duk_push_string(ctx, s); g_free(s);
        return;
    }

    /* Date, RegExp: convert to native Duktape equivalents */
    if (jsc_value_object_is_instance_of(value, "Date") ||
        jsc_value_object_is_instance_of(value, "RegExp")) {
        jsc_to_duk(ctx, pctx->ctx, value, 0);
        return;
    }

    /* Function: callable wrapper.  Use a C function as the Proxy target
       so the proxy is callable, but also supports property access for
       function-objects like lodash's _ (which is both a function and a
       namespace with hundreds of methods). */
    if (jsc_value_is_function(value)) {
        duk_push_c_function(ctx, jsc_func_call, DUK_VARARGS); /* target */
        jsc_attach_value(ctx, -1, value, pctx);

        duk_push_object(ctx);                          /* handler */
        duk_push_c_function(ctx, jsc_proxy_get, 3);
        duk_put_prop_string(ctx, -2, "get");
        duk_push_c_function(ctx, jsc_proxy_has, 2);
        duk_put_prop_string(ctx, -2, "has");
        duk_push_c_function(ctx, jsc_proxy_ownKeys, 1);
        duk_put_prop_string(ctx, -2, "ownKeys");
        duk_push_c_function(ctx, jsc_proxy_getOwnPropDesc, 2);
        duk_put_prop_string(ctx, -2, "getOwnPropertyDescriptor");

        duk_push_proxy(ctx, 0);
        return;
    }

    /* Object: wrap in Proxy */
    duk_push_object(ctx);                          /* target */
    jsc_attach_value(ctx, -1, value, pctx);

    duk_push_object(ctx);                          /* handler */
    duk_push_c_function(ctx, jsc_proxy_get, 3);
    duk_put_prop_string(ctx, -2, "get");
    duk_push_c_function(ctx, jsc_proxy_has, 2);
    duk_put_prop_string(ctx, -2, "has");
    duk_push_c_function(ctx, jsc_proxy_ownKeys, 1);
    duk_put_prop_string(ctx, -2, "ownKeys");
    duk_push_c_function(ctx, jsc_proxy_getOwnPropDesc, 2);
    duk_put_prop_string(ctx, -2, "getOwnPropertyDescriptor");

    duk_push_proxy(ctx, 0);  /* consumes target + handler */
}

/* --- JSCContext constructor --- */

static duk_ret_t jsctx_constructor(duk_context *ctx)
{
    if (!duk_is_constructor_call(ctx))
        RP_THROW(ctx, "JSCContext must be called with 'new'");

    jsc_pctx_t *pctx = jsc_pctx_new();
    if (!pctx)
        RP_THROW(ctx, "JSCContext: failed to create context");

    duk_push_this(ctx);
    duk_push_pointer(ctx, pctx);
    duk_put_prop_string(ctx, -2, JSCPCTX_SYM);
    duk_push_c_function(ctx, jscval_finalizer, 1);
    duk_set_finalizer(ctx, -2);
    duk_pop(ctx);
    return 0;
}

/* Helper: get pctx from 'this' and verify we're in the right process */
static jsc_pctx_t *jsctx_get_pctx(duk_context *ctx, const char *method)
{
    duk_push_this(ctx);
    if (!duk_get_prop_string(ctx, -1, JSCPCTX_SYM)) {
        duk_pop_2(ctx);
        RP_THROW(ctx, "JSCContext.%s(): context is invalid", method);
    }
    jsc_pctx_t *p = (jsc_pctx_t *)duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    if (!p || !p->ctx)
        RP_THROW(ctx, "JSCContext.%s(): context has been destroyed", method);
    jsc_check_pid(ctx, p);
    return p;
}

/* --- .eval(code) → wrapped result --- */

static duk_ret_t jsctx_eval(duk_context *ctx)
{
    const char *code = REQUIRE_STRING(ctx, 0,
        "JSCContext.eval(): argument must be a string");
    jsc_pctx_t *pctx = jsctx_get_pctx(ctx, "eval");

    jsc_pctx_clear(pctx);
    JSCValue *result = jsc_context_evaluate(pctx->ctx, code, -1);

    if (pctx->exception) {
        if (result) g_object_unref(result);
        jsc_check_and_throw(ctx, pctx);
        return 0;
    }

    jsc_wrap_value(ctx, pctx, result);
    g_object_unref(result);
    return 1;
}

/* --- .loadScript(path) → wrapped result --- */

static duk_ret_t jsctx_load_script(duk_context *ctx)
{
    const char *path = REQUIRE_STRING(ctx, 0,
        "JSCContext.loadScript(): argument must be a file path");
    jsc_pctx_t *pctx = jsctx_get_pctx(ctx, "loadScript");

    FILE *f = fopen(path, "r");
    if (!f) RP_THROW(ctx, "JSCContext.loadScript(): cannot open '%s'", path);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *code = (char *)malloc((size_t)len + 1);
    if (!code) { fclose(f); RP_THROW(ctx, "out of memory"); }
    size_t nr = fread(code, 1, (size_t)len, f);
    fclose(f);
    code[nr] = '\0';

    jsc_pctx_clear(pctx);
    JSCValue *result = jsc_context_evaluate_with_source_uri(
        pctx->ctx, code, (gssize)nr, path, 1);
    free(code);

    if (pctx->exception) {
        if (result) g_object_unref(result);
        jsc_check_and_throw(ctx, pctx);
        return 0;
    }

    jsc_wrap_value(ctx, pctx, result);
    g_object_unref(result);
    return 1;
}

/* --- ESM → CommonJS transform (best-effort, for bundled ESM files) ---
 *
 * Rewrites export statements to module.exports assignments.
 * Only used as a fallback when the CommonJS shim fails.
 *
 * Handles:
 *   export default <expr>         → module.exports = <expr>
 *   export function <name>(       → function <name>(  + collected
 *   export class <name>           → class <name>      + collected
 *   export const/let/var <names>  → const/let/var     + collected
 *   export { a, b, c }           → collected
 *   export { a as b }            → collected with alias
 *   import ... from '...'        → stripped (deps unavailable)
 *   import '...'                 → stripped
 *
 * Tracks quote/comment state to avoid false positives.
 * Returns a malloc'd transformed string, or NULL if no exports found.
 */

/* Append to a dynamic buffer, growing as needed */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} dbuf_t;

static void dbuf_init(dbuf_t *d, size_t initial)
{
    d->cap = initial;
    d->buf = (char *)malloc(d->cap);
    d->len = 0;
    if (d->buf) d->buf[0] = '\0';
}

static void dbuf_append(dbuf_t *d, const char *s, size_t n)
{
    if (!d->buf) return;
    if (d->len + n + 1 > d->cap) {
        d->cap = (d->len + n + 1) * 2;
        d->buf = (char *)realloc(d->buf, d->cap);
        if (!d->buf) return;
    }
    memcpy(d->buf + d->len, s, n);
    d->len += n;
    d->buf[d->len] = '\0';
}

static void dbuf_puts(dbuf_t *d, const char *s)
{
    dbuf_append(d, s, strlen(s));
}

/* Skip whitespace, return pointer to first non-space */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Read an identifier from p into buf (max buflen-1 chars), return end ptr */
static const char *read_ident(const char *p, char *buf, size_t buflen)
{
    size_t i = 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '_' || *p == '$') {
        if (i < buflen - 1) buf[i++] = *p;
        p++;
    }
    buf[i] = '\0';
    return p;
}

/* Collected export names: "module.exports.name = name;\n" appended at end */
typedef struct {
    dbuf_t names; /* accumulated assignment lines */
    int    count;
} esm_exports_t;

static void esm_collect(esm_exports_t *ex, const char *name, const char *alias)
{
    const char *local = name;
    const char *exported = alias ? alias : name;
    dbuf_puts(&ex->names, "module.exports.");
    dbuf_puts(&ex->names, exported);
    dbuf_puts(&ex->names, " = ");
    dbuf_puts(&ex->names, local);
    dbuf_puts(&ex->names, ";\n");
    ex->count++;
}

static char *esm_to_cjs(const char *src, size_t srclen)
{
    dbuf_t out;
    dbuf_init(&out, srclen + 1024);
    esm_exports_t ex;
    dbuf_init(&ex.names, 512);
    ex.count = 0;

    /* CJS prefix — wrapped in IIFE to isolate module scope */
    dbuf_puts(&out, "(function(){var module={exports:{}},exports=module.exports;\n");

    const char *p = src;
    const char *end = src + srclen;

    /* Track comment state only — quote tracking is intentionally
       omitted because regex literals (e.g. /it's/) can contain
       unmatched quotes that corrupt the parser state.  Comments
       are unambiguous (/​/ and /​* *​/) and sufficient to avoid
       false positives on commented-out export statements. */
    int in_line_comment = 0;
    int in_block_comment = 0;

    while (p < end) {
        if (in_line_comment) {
            if (*p == '\n') in_line_comment = 0;
            dbuf_append(&out, p, 1);
            p++;
            continue;
        }
        if (in_block_comment) {
            if (*p == '*' && p + 1 < end && *(p+1) == '/') {
                in_block_comment = 0;
                dbuf_append(&out, p, 2);
                p += 2;
            } else {
                dbuf_append(&out, p, 1);
                p++;
            }
            continue;
        }
        if (*p == '/' && p + 1 < end) {
            if (*(p+1) == '/') {
                in_line_comment = 1;
                dbuf_append(&out, p, 2);
                p += 2;
                continue;
            }
            if (*(p+1) == '*') {
                in_block_comment = 1;
                dbuf_append(&out, p, 2);
                p += 2;
                continue;
            }
        }

        /* --- Check for export/import at start of statement --- */
        int at_stmt = 0;
        if (p == src) {
            at_stmt = 1;
        } else {
            const char *back = p - 1;
            while (back >= src && (*back == ' ' || *back == '\t')) back--;
            if (back < src || *back == '\n' || *back == '\r' ||
                *back == ';' || *back == '{' || *back == '}')
                at_stmt = 1;
        }

        if (!at_stmt) {
            dbuf_append(&out, p, 1);
            p++;
            continue;
        }

        /* --- import ... --- */
        if (p + 6 < end && strncmp(p, "import", 6) == 0 &&
            (p[6] == ' ' || p[6] == '\t' || p[6] == '\'' || p[6] == '"')) {
            /* Skip the entire import statement/line */
            while (p < end && *p != '\n' && *p != ';') p++;
            if (p < end && *p == ';') p++;
            if (p < end && *p == '\n') p++;
            dbuf_puts(&out, "/* import stripped */\n");
            continue;
        }

        /* --- export ... --- */
        if (p + 6 < end && strncmp(p, "export", 6) == 0 &&
            (p[6] == ' ' || p[6] == '\t' || p[6] == '{')) {
            const char *after = skip_ws(p + 6);

            /* export default */
            if (strncmp(after, "default", 7) == 0 &&
                (after[7] == ' ' || after[7] == '\t' || after[7] == '\n' ||
                 after[7] == '(' || after[7] == '{')) {
                dbuf_puts(&out, "module.exports = ");
                p = skip_ws(after + 7);
                continue;
            }

            /* export function <name> */
            if (strncmp(after, "function", 8) == 0 &&
                (after[8] == ' ' || after[8] == '*')) {
                const char *np = skip_ws(after + 8);
                if (*np == '*') np = skip_ws(np + 1); /* generator */
                char name[128];
                read_ident(np, name, sizeof(name));
                if (name[0]) esm_collect(&ex, name, NULL);
                /* Emit without 'export' */
                dbuf_puts(&out, "function ");
                p = after + 8;
                continue;
            }

            /* export class <name> */
            if (strncmp(after, "class", 5) == 0 &&
                (after[5] == ' ' || after[5] == '\t')) {
                const char *np = skip_ws(after + 5);
                char name[128];
                read_ident(np, name, sizeof(name));
                if (name[0]) esm_collect(&ex, name, NULL);
                dbuf_puts(&out, "class ");
                p = after + 5;
                continue;
            }

            /* export const/let/var <name> */
            int is_decl = 0;
            int kw_len = 0;
            if (strncmp(after, "const ", 6) == 0) { is_decl = 1; kw_len = 5; }
            else if (strncmp(after, "let ", 4) == 0) { is_decl = 1; kw_len = 3; }
            else if (strncmp(after, "var ", 4) == 0) { is_decl = 1; kw_len = 3; }
            if (is_decl) {
                const char *np = skip_ws(after + kw_len);
                /* Could be destructuring: export const { a, b } = ...
                   or simple: export const name = ... */
                if (*np == '{') {
                    /* Destructuring — collect names from braces */
                    np++;
                    while (np < end && *np != '}') {
                        np = skip_ws(np);
                        char name[128];
                        np = read_ident(np, name, sizeof(name));
                        if (name[0]) esm_collect(&ex, name, NULL);
                        np = skip_ws(np);
                        if (*np == ',') np++;
                    }
                } else {
                    /* Simple or comma-separated: const a = 1, b = 2
                       Collect each name; skip values by tracking
                       nesting depth to ignore commas in expressions. */
                    for (;;) {
                        np = skip_ws(np);
                        char name[128];
                        np = read_ident(np, name, sizeof(name));
                        if (name[0]) esm_collect(&ex, name, NULL);
                        /* Skip past the value to the next comma or end */
                        int depth = 0;
                        while (np < end) {
                            if ((*np == ',' && depth == 0) ||
                                (*np == ';' && depth == 0) ||
                                (*np == '\n' && depth == 0))
                                break;
                            if (*np == '(' || *np == '[' || *np == '{')
                                depth++;
                            else if (*np == ')' || *np == ']' || *np == '}')
                                depth--;
                            else if (*np == '\'' || *np == '"' || *np == '`') {
                                char q = *np++;
                                while (np < end && *np != q) {
                                    if (*np == '\\' && np + 1 < end) np++;
                                    np++;
                                }
                            }
                            np++;
                        }
                        if (*np == ',') { np++; continue; }
                        break;
                    }
                }
                /* Emit the declaration without 'export' */
                dbuf_append(&out, after, (size_t)kw_len);
                dbuf_append(&out, " ", 1);
                p = after + kw_len;
                continue;
            }

            /* export { name1, name2, name3 as alias } */
            if (*after == '{') {
                const char *bp = after + 1;
                while (bp < end && *bp != '}') {
                    bp = skip_ws(bp);
                    char name[128];
                    bp = read_ident(bp, name, sizeof(name));
                    bp = skip_ws(bp);
                    /* Check for 'as alias' */
                    if (strncmp(bp, "as ", 3) == 0) {
                        bp = skip_ws(bp + 3);
                        char alias[128];
                        bp = read_ident(bp, alias, sizeof(alias));
                        if (name[0]) esm_collect(&ex, name, alias[0] ? alias : NULL);
                    } else {
                        if (name[0]) esm_collect(&ex, name, NULL);
                    }
                    bp = skip_ws(bp);
                    if (*bp == ',') bp++;
                }
                if (*bp == '}') bp++;
                /* Skip optional 'from "..."' (re-exports — can't resolve) */
                bp = skip_ws(bp);
                if (strncmp(bp, "from", 4) == 0) {
                    while (bp < end && *bp != '\n' && *bp != ';') bp++;
                }
                if (bp < end && *bp == ';') bp++;
                if (bp < end && *bp == '\n') bp++;
                p = bp;
                continue;
            }

            /* Unrecognized export form — pass through */
            dbuf_append(&out, p, 1);
            p++;
            continue;
        }

        /* Regular character */
        dbuf_append(&out, p, 1);
        p++;
    }

    if (ex.count == 0) {
        /* No exports found — not an ESM file */
        free(out.buf);
        free(ex.names.buf);
        return NULL;
    }

    /* Append collected export assignments + return statement */
    dbuf_puts(&out, "\n");
    if (ex.names.buf && ex.names.len > 0)
        dbuf_append(&out, ex.names.buf, ex.names.len);
    dbuf_puts(&out, "return module.exports;\n})()\n");

    free(ex.names.buf);
    return out.buf;
}

/* --- .require(path) — load script with CommonJS shim, return exports --- */

static duk_ret_t jsctx_require(duk_context *ctx)
{
    const char *path = REQUIRE_STRING(ctx, 0,
        "JSCContext.require(): argument must be a file path");
    jsc_pctx_t *pctx = jsctx_get_pctx(ctx, "require");

    FILE *f = fopen(path, "r");
    if (!f) RP_THROW(ctx, "JSCContext.require(): cannot open '%s'", path);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *raw = (char *)malloc((size_t)len + 1);
    if (!raw) { fclose(f); RP_THROW(ctx, "out of memory"); }
    size_t nr = fread(raw, 1, (size_t)len, f);
    fclose(f);
    raw[nr] = '\0';

    /* --- Attempt 1: CommonJS shim --- */
    const char *prefix = "(function(require){"
                         "var module={exports:{}},exports=module.exports;\n";
    const char *suffix = "\nreturn module.exports;\n"
                         "})(function(){})";
    size_t plen = strlen(prefix);
    size_t slen = strlen(suffix);
    char *wrapped = (char *)malloc(plen + nr + slen + 1);
    if (!wrapped) { free(raw); RP_THROW(ctx, "out of memory"); }

    memcpy(wrapped, prefix, plen);
    memcpy(wrapped + plen, raw, nr);
    memcpy(wrapped + plen + nr, suffix, slen);
    wrapped[plen + nr + slen] = '\0';

    jsc_pctx_clear(pctx);
    JSCValue *result = jsc_context_evaluate_with_source_uri(
        pctx->ctx, wrapped, (gssize)(plen + nr + slen), path, 0);
    free(wrapped);

    if (pctx->exception) {
        /* Check if it was a SyntaxError — if so, try ESM transform */
        const char *ename = jsc_exception_get_name(pctx->exception);
        int is_syntax = (ename && strcmp(ename, "SyntaxError") == 0);

        if (result) g_object_unref(result);
        result = NULL;

        if (is_syntax) {
            g_object_unref(pctx->exception);
            pctx->exception = NULL;

            /* --- Attempt 2: ESM → CJS transform --- */
            char *esm = esm_to_cjs(raw, nr);
            free(raw);
            raw = NULL;

            if (esm) {
                jsc_pctx_clear(pctx);
                result = jsc_context_evaluate_with_source_uri(
                    pctx->ctx, esm, (gssize)strlen(esm), path, 1);
                free(esm);

                if (pctx->exception) {
                    if (result) g_object_unref(result);
                    jsc_check_and_throw(ctx, pctx);
                    return 0;
                }

                jsc_wrap_value(ctx, pctx, result);
                g_object_unref(result);
                return 1;
            }
            /* esm_to_cjs returned NULL — no exports found, not ESM */
            RP_THROW(ctx, "JSCContext.require(): SyntaxError in '%s' "
                     "(not a CommonJS or ES module)", path);
        }

        /* Non-syntax error — throw it */
        free(raw);
        jsc_check_and_throw(ctx, pctx);
        return 0;
    }

    free(raw);
    jsc_wrap_value(ctx, pctx, result);
    g_object_unref(result);
    return 1;
}

/* --- .set(name, value) — set a JSC global variable --- */

static duk_ret_t jsctx_set(duk_context *ctx)
{
    const char *name = REQUIRE_STRING(ctx, 0,
        "JSCContext.set(): first argument must be a variable name");
    if (duk_get_top(ctx) < 2)
        RP_THROW(ctx, "JSCContext.set(): requires name and value");
    jsc_pctx_t *pctx = jsctx_get_pctx(ctx, "set");

    JSCValue *val = duk_to_jsc(ctx, 1, pctx);
    jsc_context_set_value(pctx->ctx, name, val);
    g_object_unref(val);
    return 0;
}

/* --- .getGlobal(name) — get a JSC global variable as a wrapped value --- */

static duk_ret_t jsctx_get_global(duk_context *ctx)
{
    const char *name = REQUIRE_STRING(ctx, 0,
        "JSCContext.getGlobal(): argument must be a variable name");
    jsc_pctx_t *pctx = jsctx_get_pctx(ctx, "getGlobal");

    JSCValue *val = jsc_context_get_value(pctx->ctx, name);
    if (!val) { duk_push_undefined(ctx); return 1; }
    jsc_wrap_value(ctx, pctx, val);
    g_object_unref(val);
    return 1;
}

/* --- .destroy() --- */

static duk_ret_t jsctx_destroy(duk_context *ctx)
{
    duk_push_this(ctx);
    if (duk_get_prop_string(ctx, -1, JSCPCTX_SYM)) {
        jsc_pctx_t *p = (jsc_pctx_t *)duk_get_pointer(ctx, -1);
        if (p) {
            jsc_pctx_unref(p);
            duk_pop(ctx);
            duk_push_pointer(ctx, NULL);
            duk_put_prop_string(ctx, -2, JSCPCTX_SYM);
        } else {
            duk_pop(ctx);
        }
    } else {
        duk_pop(ctx);
    }
    duk_pop(ctx);
    return 0;
}

/* ============================================================
   Module entry point
   ============================================================ */

duk_ret_t duk_open_module(duk_context *ctx)
{
    duk_push_object(ctx);  /* module object */
    duk_idx_t mod_idx = duk_get_top_index(ctx);

    /* Constructor: WebView(options) */
    duk_push_c_function(ctx, wv_constructor, 1);
    duk_idx_t con_idx = duk_get_top_index(ctx);

    /* Prototype object with methods */
    duk_push_object(ctx);

    duk_push_c_function(ctx, wv_set_title, 1);
    duk_put_prop_string(ctx, -2, "setTitle");

    duk_push_c_function(ctx, wv_set_size, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "setSize");

    duk_push_c_function(ctx, wv_navigate, 1);
    duk_put_prop_string(ctx, -2, "navigate");

    duk_push_c_function(ctx, wv_set_html, 1);
    duk_put_prop_string(ctx, -2, "setHtml");

    duk_push_c_function(ctx, wv_init_js, 1);
    duk_put_prop_string(ctx, -2, "init");

    duk_push_c_function(ctx, wv_eval, 1);
    duk_put_prop_string(ctx, -2, "eval");

    duk_push_c_function(ctx, wv_bind, 2);
    duk_put_prop_string(ctx, -2, "bind");

    duk_push_c_function(ctx, wv_unbind, 1);
    duk_put_prop_string(ctx, -2, "unbind");

    duk_push_c_function(ctx, wv_run, 0);
    duk_put_prop_string(ctx, -2, "run");

    duk_push_c_function(ctx, wv_terminate, 0);
    duk_put_prop_string(ctx, -2, "terminate");

    duk_push_c_function(ctx, wv_destroy, 0);
    duk_put_prop_string(ctx, -2, "destroy");

    /* Set prototype on constructor */
    duk_put_prop_string(ctx, con_idx, "prototype");

    /* Put constructor as "WebView" on module object */
    duk_put_prop_string(ctx, mod_idx, "WebView");

    /* Headless JSC one-shot evaluation */
    duk_push_c_function(ctx, jsc_exec, 1);
    duk_put_prop_string(ctx, mod_idx, "jscExec");

    /* JSCContext constructor with persistent context + proxy access */
    duk_push_c_function(ctx, jsctx_constructor, 0);
    duk_idx_t jsc_con = duk_get_top_index(ctx);

    duk_push_object(ctx); /* prototype */

    duk_push_c_function(ctx, jsctx_eval, 1);
    duk_put_prop_string(ctx, -2, "eval");

    duk_push_c_function(ctx, jsctx_load_script, 1);
    duk_put_prop_string(ctx, -2, "loadScript");

    duk_push_c_function(ctx, jsctx_require, 1);
    duk_put_prop_string(ctx, -2, "require");

    duk_push_c_function(ctx, jsctx_set, 2);
    duk_put_prop_string(ctx, -2, "set");

    duk_push_c_function(ctx, jsctx_get_global, 1);
    duk_put_prop_string(ctx, -2, "getGlobal");

    duk_push_c_function(ctx, jsctx_destroy, 0);
    duk_put_prop_string(ctx, -2, "destroy");

    duk_put_prop_string(ctx, jsc_con, "prototype");
    duk_put_prop_string(ctx, mod_idx, "JSCContext");

    /* Expose hint constants on the module object */
    duk_push_int(ctx, WEBVIEW_HINT_NONE);
    duk_put_prop_string(ctx, mod_idx, "HINT_NONE");

    duk_push_int(ctx, WEBVIEW_HINT_MIN);
    duk_put_prop_string(ctx, mod_idx, "HINT_MIN");

    duk_push_int(ctx, WEBVIEW_HINT_MAX);
    duk_put_prop_string(ctx, mod_idx, "HINT_MAX");

    duk_push_int(ctx, WEBVIEW_HINT_FIXED);
    duk_put_prop_string(ctx, mod_idx, "HINT_FIXED");

    return 1; /* return the module object */
}
