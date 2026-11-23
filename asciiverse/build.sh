#!/bin/sh
# Builds Conquest for the Asciiverse catalog into av-build/bin/:
#   conquest      the menu (asciiverse/src/conquest-menu.c): Play here,
#                 Host on the local network, Join a server
#   conquest-tty  the curses client
#   conquestd     the server
#   conqoper      the operator tool (the menu uses it to create the universe)
#   conqdriv      the universe driver, started by conquestd
#
# Upstream's autotools build is used with the OpenGL client and sound
# turned off.  The programs find their folders through CONQUEST_ETC,
# CONQUEST_STATE, CONQUEST_LIBEXEC and CONQUEST_USERDIR, which the menu
# sets, so the compiled-in prefix is never used.
#
#   sh asciiverse/build.sh
#       Linux (glibc, built on the oldest runner): ncurses, zlib,
#       libcrypt and the C++ runtime linked in, needing only libc
#   sh asciiverse/build.sh            (on macOS)
#       the system's ncurses, zlib and libc++; Homebrew's autoconf,
#       automake, libtool and bison
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build="$root/av-build"
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
# CI has no TERM; configure's curses checks need one
export TERM="${TERM:-xterm}"

cd "$root"
if [ ! -x configure ] || [ configure.ac -nt configure ] || [ src/Makefile.am -nt configure ]; then
  autoreconf -i -f >/dev/null 2>&1 || autoreconf -i -f
fi

mkdir -p "$build/obj" "$build/bin"
cd "$build/obj"
if [ "$(uname)" = Darwin ]; then
  if [ ! -f Makefile ] || [ "$root/configure" -nt Makefile ]; then
    "$root/configure" --disable-gl --disable-sound --prefix=/opt/conquest >configure.log 2>&1 \
      || { tail -40 configure.log; exit 1; }
  fi
  make -j "$jobs" >make.log 2>&1 || { tail -40 make.log; exit 1; }
  menulibs="-lncurses"
else
  # Everything but libc linked in.  configure would add -lcurses and
  # -lcrypt (the shared ones), so its checks are answered here and the
  # static archives are passed by path instead.  Without zlib the
  # server's game recordings are just not compressed.
  a() { ${CC:-cc} -print-file-name="lib$1.a"; }
  libs="$(a ncurses) $(a tinfo) $(a crypt)"
  for l in $libs; do
    [ -f "$l" ] || { echo "missing static library $l" >&2; exit 1; }
  done
  if [ ! -f Makefile ] || [ "$root/configure" -nt Makefile ]; then
    "$root/configure" --disable-gl --disable-sound --prefix=/opt/conquest \
      ac_cv_lib_curses_initscr=no ac_cv_lib_ncurses_initscr=no \
      ac_cv_lib_crypt_crypt=no ac_cv_lib_z_gzread=no \
      LIBS="$libs" LDFLAGS="-static-libstdc++ -static-libgcc" >configure.log 2>&1 \
      || { tail -40 configure.log; exit 1; }
  fi
  make -j "$jobs" >make.log 2>&1 || { tail -40 make.log; exit 1; }
  menulibs="$(a ncurses) $(a tinfo)"
fi

for p in conquest-tty conquestd conqoper conqdriv; do
  cp "src/$p" "$build/bin/$p"
done

${CC:-cc} -O2 -Wall -Wno-format-truncation -o "$build/bin/conquest" "$root/asciiverse/src/conquest-menu.c" $menulibs

strip "$build/bin/"*
echo "built: $(ls "$build/bin" | tr '\n' ' ')"
