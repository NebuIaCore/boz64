# Builds the 64-bit test APK: the original game with its 32-bit libraries moved next to the 64-bit wrapper.
#   lib/arm64-v8a/libs3e_android.so      the 64-bit wrapper (build/arm64)
#   lib/arm64-v8a/libg32_s3e_android.so  the original 32-bit engine, run by the wrapper
#   lib/arm64-v8a/lib<name>.so           the original 32-bit extensions, under their own names (the engine
#                                        finds them by listing this folder)
#   assets/boz64/blackops.dz(.dat)       the game data pack, with Mule Kick and the extra voice lines added
#                                        (tools/mulekick.py builds it into out/data)
#   assets/blackops-music/mus_mulekick_sting.mp3  Mule Kick's purchase sting (tools/mulekick_hud.py)
# The package is renamed (default com.activision.boz64) so it installs next to the original game.
# usage: python tools/package.py [--package NAME] [--label TEXT]
import argparse
import os
import secrets
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import axml  # noqa: E402
import mulekick  # noqa: E402

SDK = os.environ.get('ANDROID_HOME', r'C:\Users\nebula\AppData\Local\Android\Sdk')
BT = os.path.join(SDK, 'build-tools', '37.0.0')
NDK = os.path.join(SDK, 'ndk', '27.3.13750724')
STRIP = os.path.join(NDK, 'toolchains', 'llvm', 'prebuilt', 'windows-x86_64', 'bin', 'llvm-strip.exe')
JAVA_HOME = os.environ.get('JAVA_HOME', r'C:\Users\nebula\Desktop\desktop\ps2\jdk-21.0.12.1+1')
JAVA = os.path.join(JAVA_HOME, 'bin', 'java.exe')
JAVAC = os.path.join(JAVA_HOME, 'bin', 'javac.exe')
ANDROID_JAR = os.path.join(SDK, 'platforms', 'android-36', 'android.jar')
JAVA_SRC = os.path.join(ROOT, 'java')  # the port's own Java code, added as classes2.dex
KEYTOOL = os.path.join(JAVA_HOME, 'bin', 'keytool.exe')

BASE_APK = os.path.join(ROOT, '..', 'backup', 'apk', 'base.apk')
# The game data pack the game would otherwise download (465 MB). It goes into the APK uncompressed and
# the wrapper serves it to the game in place of Android/obb/<package>/blackops_<format>.dz. The .dz is
# tools/mulekick.py's copy of the original (DATA_DIR) with Mule Kick and the extra voice lines added.
DATA_DIR = os.path.join(ROOT, '..', 'backup', 'obb', 'com.activision.boz')
DATA_DAT = os.path.join(DATA_DIR, 'blackops_etc.dz.dat')
WRAPPER = os.path.join(ROOT, 'build', 'arm64', 'libs3e_android.so')
OUT_DIR = os.path.join(ROOT, 'out')
KEY_DIR = os.path.join(ROOT, 'keystore')
ORIGINAL_PACKAGE = 'com.activision.boz'
# Extensions left out: Google Play Games (its sign-in pop-up at start) and Google Play billing. The engine
# loads whichever extensions it finds in the lib folder, and the game treats missing ones as unavailable.
MAX_ASPECT = 4.0
# Samsung's in-app purchasing goes too: it binds its (long retired) IAP v2 service with an implicit intent, which
# throws, uncaught, for apps targeting Android 5 or later (TARGET_SDK below).
DROP_EXTENSIONS = {'libs3eGooglePlayServices.so', 'libs3eAndroidGooglePlayBilling.so', 'libs3eAndroidMarketBilling.so',
                   'libs3eSamsungInAppPurchasing.so'}
# Manifest entries left out with them:
# - Google Play, so Android never routes anything from the Play Store or Play Services to the app: the billing
#   service, receiver and purchase activity, the Play Services version, and the permissions only Google's code uses
#   (license check, billing, the account list for Play Games sign-in). Nothing else in the game uses Play Services,
#   Google's ads library or the license checker, and the expansion downloader's entry points were never declared
#   (the wrapper serves the game data itself).
# - Samsung's purchase activities.
# - Permissions the game does not need, so it asks for none: phone state (only for the device ID, which Android
#   withholds from apps anyway, and the game takes the missing permission the same way), shared storage (the game
#   only uses its own folders, which need no permission), and ones only system apps can hold.
DROP_MANIFEST = {
    'service': {'com.ideaworks3d.marmalade.s3eAndroidMarketBilling.s3eAndroidMarketBilling'},
    'receiver': {'com.ideaworks3d.marmalade.s3eAndroidMarketBilling.s3eAndroidMarketBillingReceiver'},
    'activity': {'com.ideaworks3d.marmalade.s3eAndroidGooglePlayBilling.PurchaseProxy',
                 'com.sec.android.iap.lib.activity.InboxActivity', 'com.sec.android.iap.lib.activity.PaymentActivity',
                 'com.sec.android.iap.lib.activity.ItemActivity'},
    'meta-data': {'com.google.android.gms.version'},
    'uses-permission': {'com.android.vending.CHECK_LICENSE', 'com.android.vending.BILLING',
                        'android.permission.GET_ACCOUNTS', 'android.permission.READ_PHONE_STATE',
                        'android.permission.WRITE_EXTERNAL_STORAGE', 'android.permission.BATTERY_STATS',
                        'android.permission.SET_ORIENTATION', 'android.permission.INTERACT_ACROSS_USERS_FULL'},
}
# The game targets Android 4.0 (14). Android 14 and later refuse to install apps targeting below 6.0 (23), and
# show apps below 23 a permission review screen on first launch. Below 8.0 (26), Android 13 and later also ask
# for the notification permission on first launch, because the system makes a notification channel for such apps.
# Checked for 26: no remaining service intents are implicit, files and preferences are opened private, the
# wrapper links only public system libraries, and the device ID the game uses (Android ID) is the same.
TARGET_SDK = 26

# Configuration changes the game's activity handles itself instead of being destroyed and recreated. The original
# engine can't survive being recreated (the screen stays black), and the game only lists locale, keyboardHidden,
# orientation and screenSize. A game controller counts as a keyboard, so connecting one mid-game changed
# "keyboard" and blacked the screen; dark mode switching (uiMode) and display or font changes would do the same.
MAIN_CONFIG_CHANGES = (0x0001 | 0x0002 |  # mcc, mnc (SIM)
                       0x0004 |           # locale
                       0x0008 |           # touchscreen
                       0x0010 | 0x0020 |  # keyboard, keyboardHidden
                       0x0040 |           # navigation
                       0x0080 |           # orientation
                       0x0100 |           # screenLayout
                       0x0200 |           # uiMode (dark mode, car or desk dock)
                       0x0400 | 0x0800 |  # screenSize, smallestScreenSize
                       0x1000 |           # density
                       0x2000 |           # layoutDirection
                       0x4000 |           # colorMode
                       0x40000000)        # fontScale


def run(*cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f'failed: {os.path.basename(cmd[0])}\n{r.stdout}{r.stderr}')
    return r.stdout


def keystore():
    """Signing key for the port. Made once and kept in port64/keystore so updates install over each other."""
    ks = os.path.join(KEY_DIR, 'boz64.jks')
    props = os.path.join(KEY_DIR, 'keystore.properties')
    if not os.path.exists(ks):
        os.makedirs(KEY_DIR, exist_ok=True)
        pw = secrets.token_hex(12)
        run(KEYTOOL, '-genkeypair', '-keystore', ks, '-alias', 'boz64', '-keyalg', 'RSA', '-keysize', '2048',
            '-validity', '20000', '-storepass', pw, '-keypass', pw, '-dname', 'CN=BOZ 64-bit port')
        with open(props, 'w') as f:
            f.write(f'storeFile=boz64.jks\nkeyAlias=boz64\nstorePassword={pw}\n')
    pw = dict(l.strip().split('=', 1) for l in open(props) if '=' in l)['storePassword']
    return ks, pw


def build_dex():
    """Compiles java/ into one dex file (Android 5+ loads classes2.dex next to the game's own classes.dex)."""
    work = os.path.join(OUT_DIR, 'java')
    shutil.rmtree(work, ignore_errors=True)
    classes = os.path.join(work, 'classes')
    os.makedirs(classes)
    sources = [os.path.join(d, f) for d, _, fs in os.walk(JAVA_SRC) for f in fs if f.endswith('.java')]
    run(JAVAC, '-source', '8', '-target', '8', '-Xlint:-options', '-cp', ANDROID_JAR, '-d', classes, *sources)
    class_files = [os.path.join(d, f) for d, _, fs in os.walk(classes) for f in fs if f.endswith('.class')]
    run(JAVA, '-cp', os.path.join(BT, 'lib', 'd8.jar'), 'com.android.tools.r8.D8', '--release', '--min-api', '21',
        '--lib', ANDROID_JAR, '--output', work, *class_files)
    with open(os.path.join(work, 'classes.dex'), 'rb') as f:
        return f.read()


def patch_manifest(blob, package, label):
    x = axml.AXml(blob)
    for tag, names in DROP_MANIFEST.items():
        for name in names:
            found = [e for e in x.find(tag) if e.attr('name') is not None and e.attr('name').value == name]
            if not found:
                sys.exit(f'manifest: no {tag} {name} to leave out')
            x.remove(found[0])
    m = x.find('manifest')[0]
    m.attr('package').set_string(package)
    x.find('uses-sdk')[0].attr('targetSdkVersion').set_int(TARGET_SDK)
    for e in x.elements:
        n = e.attr('name')
        if n is not None and n.ns and (n.value or '').startswith('.'):
            n.set_string(ORIGINAL_PACKAGE + n.value)  # relative class names would resolve against the new package
        if e.name == 'provider':
            a = e.attr('authorities')
            a.set_string(a.value + '.' + package.replace('.', '_'))  # authorities must be unique per device
    # Android letterboxes apps made for Android 7 and older to 1.86:1 unless they state a wider maximum. The
    # Play Games app id (unused: that extension is left out) becomes that statement: wide phones get the whole
    # screen. More square screens (tablets) were never limited.
    for e in x.find('meta-data'):
        if e.attr('name').value == 'com.google.android.gms.games.APP_ID':
            e.attr('name').set_string('android.max_aspect')
            e.attr('value').set_float(MAX_ASPECT)
    launcher = [e for e in x.find('activity') if (e.attr('name').value or '').endswith('.Main')]
    if len(launcher) != 1 or launcher[0].attr('configChanges') is None:
        sys.exit('manifest: no Main activity with configChanges')
    cc = launcher[0].attr('configChanges')
    cc.set_data(cc.data | MAIN_CONFIG_CHANGES)
    if label:  # the app and its launcher activity, so the two copies can be told apart on the home screen
        for e in x.find('application') + launcher:
            if e.attr('label') is not None:
                e.attr('label').set_string(label)
    return x.to_bytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--package', default='com.activision.boz64')
    ap.add_argument('--label', default='BO Zombies 64')
    ap.add_argument('--keep-symbols', action='store_true')
    ap.add_argument('--no-data', action='store_true', help='leave the game data out (small APK for quick tests)')
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    wrapper = os.path.join(OUT_DIR, 'libs3e_android.so')
    if args.keep_symbols:
        with open(WRAPPER, 'rb') as s, open(wrapper, 'wb') as d:
            d.write(s.read())
    else:
        run(STRIP, '--strip-unneeded', '-o', wrapper, WRAPPER)

    unsigned = os.path.join(OUT_DIR, 'boz64-unsigned.apk')
    aligned = os.path.join(OUT_DIR, 'boz64-aligned.apk')
    final = os.path.join(OUT_DIR, 'boz64.apk')
    src = zipfile.ZipFile(BASE_APK)
    with zipfile.ZipFile(unsigned, 'w') as dst:
        def put(name, data, compress):
            zi = zipfile.ZipInfo(name, date_time=(2014, 1, 1, 0, 0, 0))
            zi.compress_type = compress
            zi.external_attr = 0o644 << 16
            dst.writestr(zi, data)

        guests = []
        for info in src.infolist():
            name = info.filename
            if name.startswith('META-INF/'):
                continue  # old signature
            if name.startswith('lib/'):
                if name.endswith('.so') and os.path.basename(name) not in DROP_EXTENSIONS:
                    guests.append((os.path.basename(name), src.read(info)))
                continue
            data = src.read(info)
            if name == 'AndroidManifest.xml':
                data = patch_manifest(data, args.package, args.label)
            put(name, data, info.compress_type)
        # stored, as the other music: the engine plays it from the APK by byte range
        put(mulekick.hud.STING_ASSET, open(mulekick.hud.STING, 'rb').read(), zipfile.ZIP_STORED)
        put('classes2.dex', build_dex(), zipfile.ZIP_DEFLATED)
        put('lib/arm64-v8a/libs3e_android.so', open(wrapper, 'rb').read(), zipfile.ZIP_DEFLATED)
        for base, data in guests:
            name = 'libg32_s3e_android.so' if base == 'libs3e_android.so' else base
            put('lib/arm64-v8a/' + name, data, zipfile.ZIP_DEFLATED)
        if not args.no_data:
            for name, source in {'blackops.dz': mulekick.build(), 'blackops.dz.dat': DATA_DAT}.items():
                zi = zipfile.ZipInfo('assets/boz64/' + name, date_time=(2014, 1, 1, 0, 0, 0))
                zi.compress_type = zipfile.ZIP_STORED
                zi.external_attr = 0o644 << 16
                with open(source, 'rb') as f, dst.open(zi, 'w') as w:
                    shutil.copyfileobj(f, w, 1 << 20)

    run(os.path.join(BT, 'zipalign.exe'), '-P', '16', '-f', '4', unsigned, aligned)
    ks, pw = keystore()
    run(JAVA, '-jar', os.path.join(BT, 'lib', 'apksigner.jar'), 'sign', '--ks', ks, '--ks-key-alias', 'boz64',
        '--ks-pass', 'pass:' + pw, '--key-pass', 'pass:' + pw, '--out', final, aligned)
    os.remove(unsigned)
    os.remove(aligned)
    print(f'{final}  ({os.path.getsize(final) / 1e6:.1f} MB, package {args.package})')


if __name__ == '__main__':
    main()
