/* snapshot_macos.m — WKWebView screenshot helper for macOS
 *
 * Captures a WKWebView's content as PNG data.
 * Called from rampart-webview.c via the C function rp_wkwebview_snapshot().
 */

#ifdef __APPLE__

#import <WebKit/WebKit.h>
#import <AppKit/AppKit.h>

/* Returns a malloc'd PNG buffer.  Caller must free(). */
unsigned char *rp_wkwebview_snapshot(void *wkwebview,
                                     int full_document,
                                     size_t *out_len)
{
    WKWebView *wv = (__bridge WKWebView *)wkwebview;
    if (!wv) return NULL;

    __block NSData *pngData = nil;
    __block BOOL done = NO;

    WKSnapshotConfiguration *config = [[WKSnapshotConfiguration alloc] init];
    if (!full_document) {
        /* Visible rect only */
        config.rect = wv.bounds;
    }

    [wv takeSnapshotWithConfiguration:config
                    completionHandler:^(NSImage *image, NSError *error) {
        if (error || !image) {
            done = YES;
            return;
        }

        /* Convert NSImage → PNG data */
        NSBitmapImageRep *rep = nil;
        CGImageRef cgImage = [image CGImageForProposedRect:NULL
                                                  context:nil
                                                    hints:nil];
        if (cgImage) {
            rep = [[NSBitmapImageRep alloc] initWithCGImage:cgImage];
            pngData = [rep representationUsingType:NSBitmapImageFileTypePNG
                                        properties:@{}];
        }
        done = YES;
    }];

    /* Pump the run loop until the async snapshot completes */
    while (!done) {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }

    if (!pngData || pngData.length == 0) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    /* Copy to a malloc'd buffer (caller frees) */
    size_t len = pngData.length;
    unsigned char *result = (unsigned char *)malloc(len);
    if (!result) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    memcpy(result, pngData.bytes, len);
    if (out_len) *out_len = len;
    return result;
}

/* ================================================================
   User Agent — synchronous
   ================================================================ */

void rp_wkwebview_set_user_agent(void *wkwebview, const char *ua)
{
    WKWebView *wv = (__bridge WKWebView *)wkwebview;
    if (!wv || !ua) return;
    wv.customUserAgent = [NSString stringWithUTF8String:ua];
}

/* ================================================================
   Cookies — via WKHTTPCookieStore (async, pump run loop)
   ================================================================ */

/* Returns 1 on success, 0 on failure. */
int rp_wkwebview_set_cookie(void *wkwebview, const char *name, const char *value)
{
    WKWebView *wv = (__bridge WKWebView *)wkwebview;
    if (!wv || !name || !value) return 0;

    NSString *host = wv.URL.host;
    if (!host || host.length == 0) host = @"localhost";

    NSDictionary *props = @{
        NSHTTPCookieName:   [NSString stringWithUTF8String:name],
        NSHTTPCookieValue:  [NSString stringWithUTF8String:value],
        NSHTTPCookieDomain: host,
        NSHTTPCookiePath:   @"/"
    };
    NSHTTPCookie *cookie = [NSHTTPCookie cookieWithProperties:props];
    if (!cookie) return 0;

    WKHTTPCookieStore *store =
        wv.configuration.websiteDataStore.httpCookieStore;

    __block BOOL done = NO;
    [store setCookie:cookie completionHandler:^{ done = YES; }];

    while (!done) {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    return 1;
}

/* Pump the run loop until *done is YES. */
static void pump_until_done(BOOL *done)
{
    while (!*done) {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
}

/* Build a malloc'd JSON {name: value, ...} string from an NSArray of
   NSHTTPCookie.  Caller must free(). */
static char *cookies_to_json(NSArray<NSHTTPCookie *> *cookies)
{
    NSMutableDictionary *dict = [NSMutableDictionary dictionary];
    for (NSHTTPCookie *c in cookies) {
        if (c.name && c.value) dict[c.name] = c.value;
    }
    NSError *err = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:dict
                                                   options:0
                                                     error:&err];
    if (!data) return strdup("{}");
    NSString *json = [[NSString alloc] initWithData:data
                                           encoding:NSUTF8StringEncoding];
    const char *utf8 = [json UTF8String];
    return utf8 ? strdup(utf8) : strdup("{}");
}

/* Current page URL as a UTF-8 C string (interior NSString pointer; do
   not free; lifetime tied to the autorelease pool). */
const char *rp_wkwebview_get_uri(void *wkwebview)
{
    WKWebView *wv = (__bridge WKWebView *)wkwebview;
    if (!wv) return NULL;
    NSURL *u = wv.URL;
    if (!u) return NULL;
    NSString *s = u.absoluteString;
    return s ? [s UTF8String] : NULL;
}

/* Returns a malloc'd JSON {name: value, ...} string for cookies that
   would be sent for `uri` (host + path + secure filter).  Mirrors the
   Linux webkit_cookie_manager_get_cookies semantics.  Caller must
   free(). */
char *rp_wkwebview_get_cookies(void *wkwebview, const char *uri)
{
    WKWebView *wv = (__bridge WKWebView *)wkwebview;
    if (!wv || !uri) return NULL;

    NSURL *target = [NSURL URLWithString:[NSString stringWithUTF8String:uri]];
    if (!target) return strdup("{}");

    __block NSArray<NSHTTPCookie *> *all = nil;
    __block BOOL done = NO;
    WKHTTPCookieStore *store =
        wv.configuration.websiteDataStore.httpCookieStore;
    [store getAllCookies:^(NSArray<NSHTTPCookie *> *result) {
        all = result;
        done = YES;
    }];
    pump_until_done(&done);

    /* Filter to the cookies that would actually be sent for this URL
       (host suffix + path prefix + secure flag).  Mirrors what
       libsoup does on Linux. */
    NSString *host = target.host;
    NSString *path = target.path.length ? target.path : @"/";
    BOOL https = [target.scheme isEqualToString:@"https"];
    NSMutableArray<NSHTTPCookie *> *matching = [NSMutableArray array];
    if (host) {
        for (NSHTTPCookie *c in all) {
            NSString *cd = c.domain;
            if (!cd) continue;
            BOOL hostOK = [cd isEqualToString:host] ||
                ([cd hasPrefix:@"."] &&
                 [host hasSuffix:[cd substringFromIndex:1]]);
            if (!hostOK) continue;
            if (c.secure && !https) continue;
            NSString *cp = c.path.length ? c.path : @"/";
            if (![cp isEqualToString:@"/"] && ![path hasPrefix:cp])
                continue;
            [matching addObject:c];
        }
    }
    return cookies_to_json(matching);
}

/* Returns a malloc'd JSON {name: value, ...} string for every cookie
   in the store, no filtering.  Caller must free(). */
char *rp_wkwebview_get_all_cookies(void *wkwebview)
{
    WKWebView *wv = (__bridge WKWebView *)wkwebview;
    if (!wv) return NULL;

    __block NSArray<NSHTTPCookie *> *all = nil;
    __block BOOL done = NO;
    WKHTTPCookieStore *store =
        wv.configuration.websiteDataStore.httpCookieStore;
    [store getAllCookies:^(NSArray<NSHTTPCookie *> *result) {
        all = result;
        done = YES;
    }];
    pump_until_done(&done);

    return cookies_to_json(all);
}

#endif /* __APPLE__ */
