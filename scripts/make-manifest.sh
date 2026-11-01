#!/bin/sh
# make-manifest.sh — generate the Homebrew Channel release manifest from the
# built IPK. Same shape as webosbrew's gen-manifest: id/version/type/title
# from appinfo.json, appDescription/rootRequired from arcblock-app/manifest.json,
# sha256 + sizes from the IPK itself. ipkUrl is the bare filename so it resolves
# relative to the manifest URL (releases/latest/download/...).
# Usage: ./scripts/make-manifest.sh
# Output: arcblock-app/org.tigger2014.arcblock.manifest.json
set -eu
CD="$(cd "$(dirname "$0")/.." && pwd)"
APP="$CD/arcblock-app"
IPK="$APP"/org.tigger2014.arcblock_*_all.ipk

if [ ! -f $IPK ]; then
  echo "no IPK in $APP — run ./scripts/package-app.sh first" >&2
  exit 1
fi
# exactly one IPK expected
set -- $IPK
if [ $# -ne 1 ]; then echo "multiple IPKs in $APP — clean up first" >&2; exit 1; fi
IPK="$1"

python3 - "$APP" "$IPK" <<'PY'
import hashlib, json, os, subprocess, sys, tarfile, io
app_dir, ipk = sys.argv[1], sys.argv[2]
with open(os.path.join(app_dir, 'appinfo.json')) as f:
    appinfo = json.load(f)
try:
    with open(os.path.join(app_dir, 'manifest.json')) as f:
        desc = json.load(f)
except FileNotFoundError:
    desc = {}
with open(ipk, 'rb') as f:
    raw = f.read()
sha = hashlib.sha256(raw).hexdigest()
# installed size: sum of data.tar members (what gen-manifest reports)
installed = None
try:
    ar = subprocess.run(['ar', 'p', ipk, 'data.tar.gz'],
                        capture_output=True, check=True).stdout
    import gzip
    data = gzip.decompress(ar)
    tar = tarfile.open(fileobj=io.BytesIO(data))
    installed = sum(m.size for m in tar.getmembers() if m.isfile())
except Exception as e:
    print('warning: installed_size unavailable (%s)' % e, file=sys.stderr)
manifest = {
    'id': appinfo['id'],
    'version': appinfo['version'],
    'type': appinfo['type'],
    'title': appinfo['title'],
    'iconUri': 'https://raw.githubusercontent.com/Tigger2014/lgArcAutoBlock/main/arcblock-app/icon.png',
    'sourceUrl': 'https://github.com/Tigger2014/lgArcAutoBlock',
    'ipkUrl': os.path.basename(ipk),
    'ipkHash': {'sha256': sha},
    'ipkSize': len(raw),
}
if desc.get('appDescription'):
    manifest['appDescription'] = desc['appDescription']
if 'rootRequired' in desc:
    manifest['rootRequired'] = desc['rootRequired']
if installed is not None:
    manifest['installedSize'] = installed
out = os.path.join(app_dir, '%s.manifest.json' % appinfo['id'])
with open(out, 'w') as f:
    json.dump(manifest, f, indent=2)
    f.write('\n')
print('wrote %s' % out)
PY
