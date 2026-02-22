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
