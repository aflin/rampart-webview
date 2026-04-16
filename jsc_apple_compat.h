/*
 * jsc_apple_compat.h - GLib JSC API shim for macOS JavaScriptCore.framework
 *
 * Provides the subset of the WebKitGTK JavaScriptCore GLib API used by
 * rampart-webview, implemented on top of the macOS system
 * JavaScriptCore.framework C API.
 *
 * Only included on macOS (__APPLE__) when HAVE_JSC is defined.
 */

#ifndef JSC_APPLE_COMPAT_H
#define JSC_APPLE_COMPAT_H

#include <JavaScriptCore/JavaScriptCore.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* ==== GLib type compatibility ==== */

typedef size_t    gsize;
typedef ssize_t   gssize;
typedef void     *gpointer;
typedef unsigned  guint;
typedef char      gchar;
typedef int       gboolean;
typedef void    (*GDestroyNotify)(gpointer);

#define G_TYPE_NONE  0

/* ==== Forward declarations ==== */

typedef struct _JSCContext   JSCContext;
typedef struct _JSCValue     JSCValue;
typedef struct _JSCException JSCException;

/* ==== JSCTypedArrayType enum (matches GLib naming) ==== */

typedef enum {
    JSC_TYPED_ARRAY_NONE = -1,
    JSC_TYPED_ARRAY_INT8,
    JSC_TYPED_ARRAY_UINT8,
    JSC_TYPED_ARRAY_UINT8_CLAMPED,
    JSC_TYPED_ARRAY_INT16,
    JSC_TYPED_ARRAY_UINT16,
    JSC_TYPED_ARRAY_INT32,
    JSC_TYPED_ARRAY_UINT32,
    JSC_TYPED_ARRAY_FLOAT32,
    JSC_TYPED_ARRAY_FLOAT64
} JSCTypedArrayType;

/* ==== Object type tags for g_object_ref/unref dispatch ==== */

enum { _JSC_OBJ_CONTEXT = 0, _JSC_OBJ_VALUE = 1, _JSC_OBJ_EXCEPTION = 2 };

/* ==== Exception handler callback ==== */

typedef void (*JSCExceptionHandler)(JSCContext   *context,
                                    JSCException *exception,
                                    gpointer      user_data);

/* ==== Struct definitions ==== */

struct _JSCContext {
    int                  _type;          /* _JSC_OBJ_CONTEXT */
    int                  _refcount;
    JSGlobalContextRef   _ctx;
    JSCExceptionHandler  _exc_handler;
    gpointer             _exc_data;
    GDestroyNotify       _exc_destroy;
};

struct _JSCValue {
    int                _type;            /* _JSC_OBJ_VALUE */
    int                _refcount;
    JSCContext        *_owner;           /* ref'd */
    JSValueRef         _val;             /* protected */
};

struct _JSCException {
    int                _type;            /* _JSC_OBJ_EXCEPTION */
    int                _refcount;
    JSCContext        *_owner;           /* ref'd */
    JSValueRef         _val;             /* protected */
    char              *_cached_name;     /* lazily populated */
};

/* ==== GLib-style helpers ==== */

static void  g_free(gpointer ptr)       { free(ptr); }
static gpointer g_malloc(gsize size)    { return malloc(size); }

static void g_strfreev(gchar **str_array) {
    if (!str_array) return;
    for (int i = 0; str_array[i]; i++) free(str_array[i]);
    free(str_array);
}

/* Forward declaration (g_object_unref needs the full struct layouts) */
static gpointer g_object_ref(gpointer obj);
static void     g_object_unref(gpointer obj);

/* ==== Internal helpers ==== */

/* Helper: convert JSStringRef to malloc'd UTF-8 C string */
static char *_jsc_string_to_utf8(JSStringRef js_str) {
    if (!js_str) {
        char *e = (char *)malloc(1);
        if (e) e[0] = '\0';
        return e;
    }
    size_t max_len = JSStringGetMaximumUTF8CStringSize(js_str);
    char *result = (char *)malloc(max_len);
    if (result) JSStringGetUTF8CString(js_str, result, max_len);
    return result;
}

/* Create a new JSCValue wrapper.  Retains owner and protects val. */
static JSCValue *_jsc_value_new(JSCContext *owner, JSValueRef val) {
    JSCValue *v = (JSCValue *)calloc(1, sizeof(JSCValue));
    if (!v) return NULL;
    v->_type     = _JSC_OBJ_VALUE;
    v->_refcount = 1;
    v->_owner    = (JSCContext *)g_object_ref(owner);
    v->_val      = val;
    if (val) JSValueProtect(owner->_ctx, val);
    return v;
}

/* Create a new JSCException wrapper. */
static JSCException *_jsc_exception_new(JSCContext *owner, JSValueRef exc_val) {
    JSCException *e = (JSCException *)calloc(1, sizeof(JSCException));
    if (!e) return NULL;
    e->_type     = _JSC_OBJ_EXCEPTION;
    e->_refcount = 1;
    e->_owner    = (JSCContext *)g_object_ref(owner);
    e->_val      = exc_val;
    if (exc_val) JSValueProtect(owner->_ctx, exc_val);
    return e;
}

/* Fire the context's exception handler for exc_val. */
static void _jsc_handle_exception(JSCContext *context, JSValueRef exc_val) {
    if (!context->_exc_handler) return;
    JSCException *exc = _jsc_exception_new(context, exc_val);
    if (!exc) return;
    context->_exc_handler(context, exc, context->_exc_data);
    g_object_unref(exc);   /* handler refs it if it wants to keep it */
}

/* ==== g_object_ref / g_object_unref ==== */

static gpointer g_object_ref(gpointer obj) {
    /* All three structs start with (_type, _refcount) at the same offsets */
    int *rc = (int *)((char *)obj + sizeof(int)); /* &_refcount */
    (*rc)++;
    return obj;
}

static void g_object_unref(gpointer obj) {
    int *type = (int *)obj;
    int *rc   = (int *)((char *)obj + sizeof(int));
    if (--(*rc) > 0) return;

    switch (*type) {
    case _JSC_OBJ_CONTEXT: {
        JSCContext *c = (JSCContext *)obj;
        if (c->_exc_destroy) c->_exc_destroy(c->_exc_data);
        if (c->_ctx) JSGlobalContextRelease(c->_ctx);
        free(c);
        break;
    }
    case _JSC_OBJ_VALUE: {
        JSCValue *v = (JSCValue *)obj;
        if (v->_val) JSValueUnprotect(v->_owner->_ctx, v->_val);
        g_object_unref(v->_owner);
        free(v);
        break;
    }
    case _JSC_OBJ_EXCEPTION: {
        JSCException *e = (JSCException *)obj;
        if (e->_val) JSValueUnprotect(e->_owner->_ctx, e->_val);
        g_object_unref(e->_owner);
        free(e->_cached_name);
        free(e);
        break;
    }
    }
}

/* ================================================================
   JSCContext functions
   ================================================================ */

static JSCContext *jsc_context_new(void) {
    JSCContext *c = (JSCContext *)calloc(1, sizeof(JSCContext));
    if (!c) return NULL;
    c->_type     = _JSC_OBJ_CONTEXT;
    c->_refcount = 1;
    c->_ctx      = JSGlobalContextCreate(NULL);
    if (!c->_ctx) { free(c); return NULL; }
    return c;
}

static void jsc_context_push_exception_handler(JSCContext        *context,
                                               JSCExceptionHandler handler,
                                               gpointer            data,
                                               GDestroyNotify      destroy) {
    if (context->_exc_destroy) context->_exc_destroy(context->_exc_data);
    context->_exc_handler = handler;
    context->_exc_data    = data;
    context->_exc_destroy = destroy;
}

static JSCValue *jsc_context_evaluate(JSCContext *context,
                                      const char *code, gssize length) {
    (void)length;
    JSStringRef script = JSStringCreateWithUTF8CString(code);
    JSValueRef  exception = NULL;
    JSValueRef  result = JSEvaluateScript(context->_ctx, script,
                                          NULL, NULL, 0, &exception);
    JSStringRelease(script);
    if (exception) {
        _jsc_handle_exception(context, exception);
        return NULL;
    }
    if (!result) result = JSValueMakeUndefined(context->_ctx);
    return _jsc_value_new(context, result);
}

static JSCValue *jsc_context_evaluate_with_source_uri(
        JSCContext *context, const char *code, gssize length,
        const char *uri, unsigned line) {
    (void)length;
    JSStringRef script     = JSStringCreateWithUTF8CString(code);
    JSStringRef source_uri = uri ? JSStringCreateWithUTF8CString(uri) : NULL;
    JSValueRef  exception  = NULL;
    JSValueRef  result = JSEvaluateScript(context->_ctx, script,
                                          NULL, source_uri,
                                          (int)line, &exception);
    JSStringRelease(script);
    if (source_uri) JSStringRelease(source_uri);
    if (exception) {
        _jsc_handle_exception(context, exception);
        return NULL;
    }
    if (!result) result = JSValueMakeUndefined(context->_ctx);
    return _jsc_value_new(context, result);
}

static void jsc_context_set_value(JSCContext *context,
                                  const char *name, JSCValue *value) {
    JSObjectRef global  = JSContextGetGlobalObject(context->_ctx);
    JSStringRef js_name = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(context->_ctx, global, js_name, value->_val,
                        kJSPropertyAttributeNone, NULL);
    JSStringRelease(js_name);
}

static JSCValue *jsc_context_get_value(JSCContext *context, const char *name) {
    JSObjectRef global  = JSContextGetGlobalObject(context->_ctx);
    JSStringRef js_name = JSStringCreateWithUTF8CString(name);
    JSValueRef  val     = JSObjectGetProperty(context->_ctx, global, js_name, NULL);
    JSStringRelease(js_name);
    if (!val) val = JSValueMakeUndefined(context->_ctx);
    return _jsc_value_new(context, val);
}

/* ================================================================
   JSCValue creation
   ================================================================ */

static JSCValue *jsc_value_new_undefined(JSCContext *context) {
    return _jsc_value_new(context, JSValueMakeUndefined(context->_ctx));
}

static JSCValue *jsc_value_new_null(JSCContext *context) {
    return _jsc_value_new(context, JSValueMakeNull(context->_ctx));
}

static JSCValue *jsc_value_new_boolean(JSCContext *context, gboolean value) {
    return _jsc_value_new(context, JSValueMakeBoolean(context->_ctx, value));
}

static JSCValue *jsc_value_new_number(JSCContext *context, double value) {
    return _jsc_value_new(context, JSValueMakeNumber(context->_ctx, value));
}

static JSCValue *jsc_value_new_string(JSCContext *context, const char *str) {
    JSStringRef js_str = JSStringCreateWithUTF8CString(str);
    JSValueRef  val    = JSValueMakeString(context->_ctx, js_str);
    JSStringRelease(js_str);
    return _jsc_value_new(context, val);
}

/* Bridge for ArrayBuffer deallocator: GDestroyNotify → JSC deallocator */
typedef struct {
    GDestroyNotify destroy;
    gpointer       user_data;
} _jsc_dealloc_info;

static void _jsc_arraybuf_dealloc(void *bytes, void *deallocator_ctx) {
    (void)bytes;
    _jsc_dealloc_info *info = (_jsc_dealloc_info *)deallocator_ctx;
    if (info && info->destroy) info->destroy(info->user_data);
    free(info);
}

static JSCValue *jsc_value_new_array_buffer(JSCContext *context,
        gpointer data, gsize size, GDestroyNotify destroy, gpointer user_data) {
    _jsc_dealloc_info *info = (_jsc_dealloc_info *)malloc(sizeof(*info));
    if (!info) { if (destroy) destroy(user_data); return NULL; }
    info->destroy   = destroy;
    info->user_data = user_data;

    JSValueRef exception = NULL;
    JSObjectRef ab = JSObjectMakeArrayBufferWithBytesNoCopy(
        context->_ctx, data, size, _jsc_arraybuf_dealloc, info, &exception);
    if (exception || !ab) {
        /* deallocator was NOT called — clean up manually */
        if (destroy) destroy(user_data);
        free(info);
        return NULL;
    }
    return _jsc_value_new(context, (JSValueRef)ab);
}

/* ================================================================
   JSCValue type checking
   ================================================================ */

static gboolean jsc_value_is_undefined(JSCValue *value) {
    return JSValueIsUndefined(value->_owner->_ctx, value->_val);
}
static gboolean jsc_value_is_null(JSCValue *value) {
    return JSValueIsNull(value->_owner->_ctx, value->_val);
}
static gboolean jsc_value_is_boolean(JSCValue *value) {
    return JSValueIsBoolean(value->_owner->_ctx, value->_val);
}
static gboolean jsc_value_is_number(JSCValue *value) {
    return JSValueIsNumber(value->_owner->_ctx, value->_val);
}
static gboolean jsc_value_is_string(JSCValue *value) {
    return JSValueIsString(value->_owner->_ctx, value->_val);
}
static gboolean jsc_value_is_object(JSCValue *value) {
    return JSValueIsObject(value->_owner->_ctx, value->_val);
}
static gboolean jsc_value_is_array(JSCValue *value) {
    return JSValueIsArray(value->_owner->_ctx, value->_val);
}
static gboolean jsc_value_is_function(JSCValue *value) {
    if (!JSValueIsObject(value->_owner->_ctx, value->_val)) return 0;
    JSObjectRef obj = JSValueToObject(value->_owner->_ctx, value->_val, NULL);
    return obj ? JSObjectIsFunction(value->_owner->_ctx, obj) : 0;
}
static gboolean jsc_value_is_typed_array(JSCValue *value) {
    JSTypedArrayType t = JSValueGetTypedArrayType(
        value->_owner->_ctx, value->_val, NULL);
    return (t != kJSTypedArrayTypeNone && t != kJSTypedArrayTypeArrayBuffer);
}
static gboolean jsc_value_is_array_buffer(JSCValue *value) {
    JSTypedArrayType t = JSValueGetTypedArrayType(
        value->_owner->_ctx, value->_val, NULL);
    return (t == kJSTypedArrayTypeArrayBuffer);
}

/* ================================================================
   JSCValue conversion to C types
   ================================================================ */

static gboolean jsc_value_to_boolean(JSCValue *value) {
    return (gboolean)JSValueToBoolean(value->_owner->_ctx, value->_val);
}
static double jsc_value_to_double(JSCValue *value) {
    return JSValueToNumber(value->_owner->_ctx, value->_val, NULL);
}
static int jsc_value_to_int32(JSCValue *value) {
    return (int)JSValueToNumber(value->_owner->_ctx, value->_val, NULL);
}
static char *jsc_value_to_string(JSCValue *value) {
    JSStringRef js_str = JSValueToStringCopy(
        value->_owner->_ctx, value->_val, NULL);
    char *result = _jsc_string_to_utf8(js_str);
    if (js_str) JSStringRelease(js_str);
    return result;
}

/* ================================================================
   JSCValue object property operations
   ================================================================ */

static JSCValue *jsc_value_object_get_property(JSCValue *value,
                                                const char *name) {
    JSObjectRef obj = JSValueToObject(value->_owner->_ctx, value->_val, NULL);
    if (!obj)
        return _jsc_value_new(value->_owner,
                              JSValueMakeUndefined(value->_owner->_ctx));
    JSStringRef js_name = JSStringCreateWithUTF8CString(name);
    JSValueRef  val = JSObjectGetProperty(value->_owner->_ctx, obj,
                                          js_name, NULL);
    JSStringRelease(js_name);
    if (!val) val = JSValueMakeUndefined(value->_owner->_ctx);
    return _jsc_value_new(value->_owner, val);
}

static void jsc_value_object_set_property(JSCValue *obj_val,
                                          const char *name, JSCValue *value) {
    JSObjectRef obj = JSValueToObject(obj_val->_owner->_ctx,
                                      obj_val->_val, NULL);
    if (!obj) return;
    JSStringRef js_name = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(obj_val->_owner->_ctx, obj, js_name, value->_val,
                        kJSPropertyAttributeNone, NULL);
    JSStringRelease(js_name);
}

static gboolean jsc_value_object_has_property(JSCValue *value,
                                              const char *name) {
    JSObjectRef obj = JSValueToObject(value->_owner->_ctx, value->_val, NULL);
    if (!obj) return 0;
    JSStringRef js_name = JSStringCreateWithUTF8CString(name);
    gboolean    result  = JSObjectHasProperty(value->_owner->_ctx, obj,
                                              js_name);
    JSStringRelease(js_name);
    return result;
}

static JSCValue *jsc_value_object_get_property_at_index(JSCValue *value,
                                                        guint index) {
    JSObjectRef obj = JSValueToObject(value->_owner->_ctx, value->_val, NULL);
    if (!obj)
        return _jsc_value_new(value->_owner,
                              JSValueMakeUndefined(value->_owner->_ctx));
    JSValueRef val = JSObjectGetPropertyAtIndex(value->_owner->_ctx, obj,
                                                index, NULL);
    if (!val) val = JSValueMakeUndefined(value->_owner->_ctx);
    return _jsc_value_new(value->_owner, val);
}

static void jsc_value_object_set_property_at_index(JSCValue *obj_val,
                                                   guint index,
                                                   JSCValue *value) {
    JSObjectRef obj = JSValueToObject(obj_val->_owner->_ctx,
                                      obj_val->_val, NULL);
    if (!obj) return;
    JSObjectSetPropertyAtIndex(obj_val->_owner->_ctx, obj, index,
                               value->_val, NULL);
}

static gboolean jsc_value_object_is_instance_of(JSCValue *value,
                                                const char *name) {
    if (!JSValueIsObject(value->_owner->_ctx, value->_val)) return 0;
    JSObjectRef global   = JSContextGetGlobalObject(value->_owner->_ctx);
    JSStringRef js_name  = JSStringCreateWithUTF8CString(name);
    JSValueRef  ctor_val = JSObjectGetProperty(value->_owner->_ctx, global,
                                               js_name, NULL);
    JSStringRelease(js_name);
    if (!ctor_val || !JSValueIsObject(value->_owner->_ctx, ctor_val)) return 0;
    JSObjectRef ctor = JSValueToObject(value->_owner->_ctx, ctor_val, NULL);
    if (!ctor) return 0;
    return JSValueIsInstanceOfConstructor(value->_owner->_ctx, value->_val,
                                          ctor, NULL);
}

static gchar **jsc_value_object_enumerate_properties(JSCValue *value) {
    if (!JSValueIsObject(value->_owner->_ctx, value->_val)) return NULL;
    JSObjectRef obj = JSValueToObject(value->_owner->_ctx, value->_val, NULL);
    if (!obj) return NULL;

    JSPropertyNameArrayRef names =
        JSObjectCopyPropertyNames(value->_owner->_ctx, obj);
    size_t count = JSPropertyNameArrayGetCount(names);
    if (count == 0) {
        JSPropertyNameArrayRelease(names);
        return NULL;
    }

    gchar **result = (gchar **)malloc(sizeof(gchar *) * (count + 1));
    if (!result) { JSPropertyNameArrayRelease(names); return NULL; }

    for (size_t i = 0; i < count; i++) {
        JSStringRef n = JSPropertyNameArrayGetNameAtIndex(names, i);
        result[i] = _jsc_string_to_utf8(n);
    }
    result[count] = NULL;
    JSPropertyNameArrayRelease(names);
    return result;
}

/* ==== Method invocation / function calls ==== */

static JSCValue *jsc_value_object_invoke_methodv(JSCValue   *obj_val,
                                                 const char *method_name,
                                                 guint       n_args,
                                                 JSCValue  **args) {
    JSGlobalContextRef ctx = obj_val->_owner->_ctx;
    JSObjectRef obj = JSValueToObject(ctx, obj_val->_val, NULL);
    if (!obj)
        return _jsc_value_new(obj_val->_owner, JSValueMakeUndefined(ctx));

    JSStringRef js_name   = JSStringCreateWithUTF8CString(method_name);
    JSValueRef  method_vr = JSObjectGetProperty(ctx, obj, js_name, NULL);
    JSStringRelease(js_name);
    if (!method_vr || !JSValueIsObject(ctx, method_vr))
        return _jsc_value_new(obj_val->_owner, JSValueMakeUndefined(ctx));

    JSObjectRef method = JSValueToObject(ctx, method_vr, NULL);
    if (!method || !JSObjectIsFunction(ctx, method))
        return _jsc_value_new(obj_val->_owner, JSValueMakeUndefined(ctx));

    JSValueRef *js_args = NULL;
    if (n_args > 0) {
        js_args = (JSValueRef *)malloc(sizeof(JSValueRef) * n_args);
        if (!js_args) return NULL;
        for (guint i = 0; i < n_args; i++)
            js_args[i] = args[i]->_val;
    }

    JSValueRef exception = NULL;
    JSValueRef result = JSObjectCallAsFunction(ctx, method, obj,
                                               n_args, js_args, &exception);
    free(js_args);

    if (exception) {
        _jsc_handle_exception(obj_val->_owner, exception);
        return NULL;
    }
    if (!result) result = JSValueMakeUndefined(ctx);
    return _jsc_value_new(obj_val->_owner, result);
}

/* Variadic version — only used with G_TYPE_NONE (no arguments) */
static JSCValue *jsc_value_object_invoke_method(JSCValue *value,
                                                const char *method, ...) {
    va_list ap;
    va_start(ap, method);
    va_end(ap);
    return jsc_value_object_invoke_methodv(value, method, 0, NULL);
}

static JSCValue *jsc_value_function_callv(JSCValue  *func,
                                          guint      n_args,
                                          JSCValue **args) {
    JSGlobalContextRef ctx = func->_owner->_ctx;
    if (!JSValueIsObject(ctx, func->_val))
        return _jsc_value_new(func->_owner, JSValueMakeUndefined(ctx));

    JSObjectRef fn_obj = JSValueToObject(ctx, func->_val, NULL);
    if (!fn_obj || !JSObjectIsFunction(ctx, fn_obj))
        return _jsc_value_new(func->_owner, JSValueMakeUndefined(ctx));

    JSValueRef *js_args = NULL;
    if (n_args > 0) {
        js_args = (JSValueRef *)malloc(sizeof(JSValueRef) * n_args);
        if (!js_args) return NULL;
        for (guint i = 0; i < n_args; i++)
            js_args[i] = args[i]->_val;
    }

    JSValueRef exception = NULL;
    JSValueRef result = JSObjectCallAsFunction(ctx, fn_obj, NULL,
                                               n_args, js_args, &exception);
    free(js_args);

    if (exception) {
        _jsc_handle_exception(func->_owner, exception);
        return NULL;
    }
    if (!result) result = JSValueMakeUndefined(ctx);
    return _jsc_value_new(func->_owner, result);
}

/* ================================================================
   Typed array operations
   ================================================================ */

static JSCTypedArrayType jsc_value_typed_array_get_type(JSCValue *value) {
    JSTypedArrayType t = JSValueGetTypedArrayType(
        value->_owner->_ctx, value->_val, NULL);
    switch (t) {
    case kJSTypedArrayTypeInt8Array:         return JSC_TYPED_ARRAY_INT8;
    case kJSTypedArrayTypeUint8Array:        return JSC_TYPED_ARRAY_UINT8;
    case kJSTypedArrayTypeUint8ClampedArray: return JSC_TYPED_ARRAY_UINT8_CLAMPED;
    case kJSTypedArrayTypeInt16Array:        return JSC_TYPED_ARRAY_INT16;
    case kJSTypedArrayTypeUint16Array:       return JSC_TYPED_ARRAY_UINT16;
    case kJSTypedArrayTypeInt32Array:        return JSC_TYPED_ARRAY_INT32;
    case kJSTypedArrayTypeUint32Array:       return JSC_TYPED_ARRAY_UINT32;
    case kJSTypedArrayTypeFloat32Array:      return JSC_TYPED_ARRAY_FLOAT32;
    case kJSTypedArrayTypeFloat64Array:      return JSC_TYPED_ARRAY_FLOAT64;
    default:                                 return JSC_TYPED_ARRAY_NONE;
    }
}

static gpointer jsc_value_typed_array_get_data(JSCValue *value,
                                               gsize *length) {
    JSObjectRef obj = JSValueToObject(value->_owner->_ctx, value->_val, NULL);
    if (!obj) { if (length) *length = 0; return NULL; }
    if (length)
        *length = JSObjectGetTypedArrayLength(value->_owner->_ctx, obj, NULL);
    return JSObjectGetTypedArrayBytesPtr(value->_owner->_ctx, obj, NULL);
}

static gsize jsc_value_typed_array_get_size(JSCValue *value) {
    JSObjectRef obj = JSValueToObject(value->_owner->_ctx, value->_val, NULL);
    if (!obj) return 0;
    return JSObjectGetTypedArrayByteLength(value->_owner->_ctx, obj, NULL);
}

/* ================================================================
   ArrayBuffer operations
   ================================================================ */

static gpointer jsc_value_array_buffer_get_data(JSCValue *value,
                                                 gsize *size) {
    JSObjectRef obj = JSValueToObject(value->_owner->_ctx, value->_val, NULL);
    if (!obj) { if (size) *size = 0; return NULL; }
    if (size)
        *size = JSObjectGetArrayBufferByteLength(value->_owner->_ctx, obj, NULL);
    return JSObjectGetArrayBufferBytesPtr(value->_owner->_ctx, obj, NULL);
}

/* ================================================================
   JSCException functions
   ================================================================ */

static const char *jsc_exception_get_name(JSCException *exception) {
    if (exception->_cached_name) return exception->_cached_name;

    JSGlobalContextRef ctx = exception->_owner->_ctx;
    JSObjectRef obj = JSValueToObject(ctx, exception->_val, NULL);
    if (!obj) { exception->_cached_name = strdup("Error"); return exception->_cached_name; }

    JSStringRef name_key = JSStringCreateWithUTF8CString("name");
    JSValueRef  name_val = JSObjectGetProperty(ctx, obj, name_key, NULL);
    JSStringRelease(name_key);

    if (!name_val || !JSValueIsString(ctx, name_val)) {
        exception->_cached_name = strdup("Error");
        return exception->_cached_name;
    }

    JSStringRef js_str = JSValueToStringCopy(ctx, name_val, NULL);
    exception->_cached_name = _jsc_string_to_utf8(js_str);
    if (js_str) JSStringRelease(js_str);
    return exception->_cached_name ? exception->_cached_name : "Error";
}

static char *jsc_exception_report(JSCException *exception) {
    JSGlobalContextRef ctx = exception->_owner->_ctx;
    JSObjectRef obj = JSValueToObject(ctx, exception->_val, NULL);

    /* If the exception isn't an object, just stringify it */
    if (!obj) {
        JSStringRef s = JSValueToStringCopy(ctx, exception->_val, NULL);
        char *result = _jsc_string_to_utf8(s);
        if (s) JSStringRelease(s);
        return result ? result : strdup("Unknown error");
    }

    /* Get message */
    char *msg = NULL;
    {
        JSStringRef key = JSStringCreateWithUTF8CString("message");
        JSValueRef  val = JSObjectGetProperty(ctx, obj, key, NULL);
        JSStringRelease(key);
        if (val && JSValueIsString(ctx, val)) {
            JSStringRef s = JSValueToStringCopy(ctx, val, NULL);
            msg = _jsc_string_to_utf8(s);
            if (s) JSStringRelease(s);
        }
    }

    /* Get line number */
    int line = 0;
    {
        JSStringRef key = JSStringCreateWithUTF8CString("line");
        JSValueRef  val = JSObjectGetProperty(ctx, obj, key, NULL);
        JSStringRelease(key);
        if (val && JSValueIsNumber(ctx, val))
            line = (int)JSValueToNumber(ctx, val, NULL);
    }

    /* Get sourceURL */
    char *url = NULL;
    {
        JSStringRef key = JSStringCreateWithUTF8CString("sourceURL");
        JSValueRef  val = JSObjectGetProperty(ctx, obj, key, NULL);
        JSStringRelease(key);
        if (val && JSValueIsString(ctx, val)) {
            JSStringRef s = JSValueToStringCopy(ctx, val, NULL);
            url = _jsc_string_to_utf8(s);
            if (s) JSStringRelease(s);
        }
    }

    const char *name = jsc_exception_get_name(exception);
    size_t len = strlen(name) + (msg ? strlen(msg) : 7) +
                 (url ? strlen(url) : 0) + 64;
    char *report = (char *)malloc(len);
    if (!report) { free(msg); free(url); return strdup("Error"); }

    if (url && line > 0)
        snprintf(report, len, "%s:%d: %s: %s", url, line, name,
                 msg ? msg : "Unknown");
    else if (line > 0)
        snprintf(report, len, "Line %d: %s: %s", line, name,
                 msg ? msg : "Unknown");
    else
        snprintf(report, len, "%s: %s", name, msg ? msg : "Unknown");

    free(msg);
    free(url);
    return report;
}

#endif /* JSC_APPLE_COMPAT_H */
