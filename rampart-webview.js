/* rampart-webview.js -- pick the rampart-webview build matching this host's
 * WebKitGTK.
 *
 * Installed ONLY by the Linux docker ("oven") builds, which ship two variants:
 *
 *     rampart-webview_wk41.so    webkit2gtk-4.1  (GTK3 + libsoup3)
 *     rampart-webview_wk40.so    webkit2gtk-4.0  (GTK3 + libsoup2)
 *
 * 4.0 and 4.1 expose the SAME WebKit API and both use GTK3.  They differ only
 * in which sonames the .so NEEDs:
 *
 *     libwebkit2gtk-4.0.so.37 / libsoup-2.4.so.1 / libjavascriptcoregtk-4.0.so.18
 *     libwebkit2gtk-4.1.so.0  / libsoup-3.0.so.0 / libjavascriptcoregtk-4.1.so.0
 *
 * No single build works everywhere: Ubuntu 24.04 dropped 4.0 entirely, while
 * older distros ship only 4.0.  And the choice cannot be made at install time,
 * because `rampart --install all` normally runs BEFORE WebKit is installed --
 * there would be nothing to detect.  So it is made here, at first require(),
 * by which point the user has installed WebKit.  Keeping both .so files (176K
 * each) also means a distro upgrade that swaps 4.0 for 4.1 just works, with no
 * reinstall.
 *
 * macOS, FreeBSD and native (non-docker) Linux builds install a plain
 * rampart-webview.so and never see this file.
 */

/* Preference order: 4.1 first -- it is the variant that survives on modern
   distros, and where both exist (Debian 12, Ubuntu 22.04) 4.0 is the one being
   removed.  A failed dlopen on a missing soname fails before anything is
   loaded, so trying 4.1 first costs essentially nothing on a 4.0-only box. */
var variants = ["rampart-webview_wk41", "rampart-webview_wk40"];

/* Best-effort: name the exact package for this distro.  Purely a message
   nicety -- any failure here falls through to the generic hint. */
function distroHint() {
    var id = "", ver = "";

    try {
        var txt = rampart.utils.readFile("/etc/os-release", { retString: true });
        var m = /^ID=("?)([^"\n]*)\1/m.exec(txt);
        if (m) id = m[2];
        m = /^VERSION_ID=("?)([^"\n]*)\1/m.exec(txt);
        if (m) ver = m[2];
    } catch (e) { /* no /etc/os-release, or unreadable -- use the generic hint */ }

    var major = parseFloat(ver);

    if (id === "debian")
        return (major >= 12) ? "    sudo apt install libwebkit2gtk-4.1-0"
                             : "    sudo apt install libwebkit2gtk-4.0-37";
    if (id === "ubuntu")
        return (major >= 22.04) ? "    sudo apt install libwebkit2gtk-4.1-0"
                                : "    sudo apt install libwebkit2gtk-4.0-37";
    if (id === "fedora" || id === "rhel" || id === "centos" ||
        id === "rocky"  || id === "almalinux")
        return "    sudo dnf install webkit2gtk4.1";
    if (id === "arch" || id === "manjaro")
        return "    sudo pacman -S webkit2gtk-4.1";
    if (id === "alpine")
        return "    sudo apk add webkit2gtk";
    if (id === "opensuse-leap" || id === "opensuse-tumbleweed" || id === "sles")
        return "    sudo zypper install libwebkit2gtk-4_1-0";

    return "    Debian/Ubuntu:  sudo apt install libwebkit2gtk-4.1-0  (older: -4.0-37)\n" +
           "    Fedora/RHEL:    sudo dnf install webkit2gtk4.1";
}

var mod = null, errors = [];

for (var i = 0; i < variants.length && !mod; i++) {
    try {
        mod = require(variants[i]);
    } catch (e) {
        errors.push("    " + variants[i] + ".so -- " +
                    String(e.message).replace(/\s+$/, ""));
    }
}

if (mod) {
    module.exports = mod;
} else {
    throw new Error(
        "rampart-webview: no usable WebKitGTK found.\n\n" +
        "  This module needs WebKitGTK (GTK3) from your distribution:\n" +
        distroHint() + "\n\n" +
        "  Neither shipped variant could load:\n" +
        errors.join("\n") + "\n"
    );
}
