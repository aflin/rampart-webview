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

#endif /* __APPLE__ */
