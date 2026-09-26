#!/bin/sh
# Release notes for a version tag: quick install instructions, then the
# changelog, which is the tagged commit's message. The Build workflow uses
# this for the GitHub release.
# Usage: scripts/release-notes.sh <tag>        e.g. scripts/release-notes.sh v1.1.0
set -eu
TAG=$1
VERSION=${TAG#v}
IPK="com.lginputmapper.app_${VERSION}_all.ipk"

cat <<EOF
## Install

Needs a rooted TV with [Homebrew Channel](https://github.com/webosbrew/webos-homebrew-channel) (root status OK).

**Homebrew Channel:** Settings → **Add repository**, enter
\`https://raw.githubusercontent.com/fivefold3/webos-homebrew-repo/main/repo.json\`,
then install **LG Input Mapper** from the list. Updates show up there too.

**Manually over SSH:** download \`$IPK\` below, then

\`\`\`sh
ssh root@<tv-ip> 'cat > /tmp/$IPK' < $IPK
ssh root@<tv-ip> "luna-send -n 1 luna://com.webos.appInstallService/dev/install '{\"id\":\"com.ares.defaultName\",\"ipkUrl\":\"/tmp/$IPK\",\"subscribe\":true}' </dev/null"
\`\`\`

The app appears in the launcher a few seconds later.

Either way, open LG Input Mapper once afterwards: it sets itself up and
starts at boot from then on.

## What's new in $VERSION

EOF

# The commit message: title in bold, section names ("Mappings") as headings,
# no Co-Authored-By trailers.
# (in a tag build HEAD is the tagged commit, should the tag itself be missing)
REV=$(git rev-parse -q --verify "$TAG^{commit}" || git rev-parse HEAD)
git log -1 --format=%B "$REV" | awk '
    /^Co-Authored-By:/ { next }
    { lines[++n] = $0 }
    END {
        while (n > 0 && lines[n] == "") n--
        print "**" lines[1] "**"
        for (i = 2; i <= n; i++) {
            l = lines[i]
            if (l != "" && l !~ /^(- |  )/ && lines[i + 1] ~ /^- /) print "### " l
            else print l
        }
    }'
