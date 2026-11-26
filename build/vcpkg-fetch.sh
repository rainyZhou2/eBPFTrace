#!/bin/sh
# vcpkg asset source (X_VCPKG_ASSET_SOURCES=x-script): routes GitHub downloads
# through GITHUB_PROXY, sourceware downloads through SOURCEWARE_MIRROR, and
# GNU downloads through GNU_MIRROR when the origins are not reachable at a
# usable speed.
set -eu
url="$1"
dst="$2"
case "$url" in
https://github.com/* | https://raw.githubusercontent.com/* | https://objects.githubusercontent.com/*)
	if [ -n "${GITHUB_PROXY:-}" ]; then
		url="${GITHUB_PROXY%/}/$url"
	fi
	;;
https://sourceware.org/pub/*)
	if [ -n "${SOURCEWARE_MIRROR:-}" ]; then
		url="${SOURCEWARE_MIRROR%/}/${url#https://sourceware.org/pub/}"
	fi
	;;
https://ftpmirror.gnu.org/gnu/*)
	if [ -n "${GNU_MIRROR:-}" ]; then
		url="${GNU_MIRROR%/}/${url#https://ftpmirror.gnu.org/gnu/}"
	fi
	;;
https://ftp.gnu.org/gnu/*)
	if [ -n "${GNU_MIRROR:-}" ]; then
		url="${GNU_MIRROR%/}/${url#https://ftp.gnu.org/gnu/}"
	fi
	;;
https://www.mirrorservice.org/sites/ftp.gnu.org/gnu/*)
	if [ -n "${GNU_MIRROR:-}" ]; then
		url="${GNU_MIRROR%/}/${url#https://www.mirrorservice.org/sites/ftp.gnu.org/gnu/}"
	fi
	;;
esac
exec curl -fsSL --retry 5 --retry-delay 3 --connect-timeout 20 --speed-limit 10240 --speed-time 60 -o "$dst" "$url"
