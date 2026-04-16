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

/* Returns a malloc'd JSON string of {name: value, ...} for cookies
   matching the current page's host.  Caller must free(). */
char *rp_wkwebview_get_cookies(void *wkwebview)
{
    WKWebView *wv = (__bridge WKWebView *)wkwebview;
    if (!wv) return NULL;

    NSString *host = wv.URL.host;
    __block NSArray<NSHTTPCookie *> *cookies = nil;
    __block BOOL done = NO;

    WKHTTPCookieStore *store =
        wv.configuration.websiteDataStore.httpCookieStore;
    [store getAllCookies:^(NSArray<NSHTTPCookie *> *result) {
        cookies = result;
        done = YES;
    }];

    while (!done) {
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }

    /* Build a dict of name→value.  We return all cookies in the store
       for simplicity; the Linux implementation filters by URI but that
       doesn't round-trip with setHtml pages, so behavior is aligned
       by returning everything. */
    (void)host;
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

#endif /* __APPLE__ */
