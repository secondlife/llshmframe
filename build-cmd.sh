#!/usr/bin/env bash

cd "$(dirname "$0")"

# turn on verbose debugging output for parabuild logs.
exec 4>&1; export BASH_XTRACEFD=4; set -x

# make errors fatal
set -e

# bleat on references to undefined shell variables
set -u

# Check autobuild is around or fail
if [ -z "$AUTOBUILD" ] ; then
    exit 1
fi

if [ "$OSTYPE" = "cygwin" ] ; then
    autobuild="$(cygpath -u $AUTOBUILD)"
else
    autobuild="$AUTOBUILD"
fi

top="$(pwd)"
stage="$(pwd)/stage"

# load autobuild provided shell functions and variables
source_environment_tempfile="$stage/source_environment.sh"
mkdir -p "$stage"
"$autobuild" source_environment > "$source_environment_tempfile"
. "$source_environment_tempfile"

# remove_cxxstd
source "$(dirname "$AUTOBUILD_VARIABLES_FILE")/functions"

# the CMakeLists.txt project() version -- kept in lock-step with it by hand,
# same as Dullahan's own VERSION.txt approach (just without needing a
# compiled version-probe helper, since this doesn't generate a runtime
# version header the way Dullahan's CEF-derived version string does).
version="$(grep -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1 | cut -d' ' -f2)"

case "$AUTOBUILD_PLATFORM" in
    windows*)
        load_vsvars

        cd "$stage"
        cmake .. \
            -G "$AUTOBUILD_WIN_CMAKE_GEN" -A "$AUTOBUILD_WIN_VSPLATFORM" \
            -DCMAKE_INSTALL_PREFIX="$(cygpath -m "$stage")" \
            -DCMAKE_INSTALL_LIBDIR=lib/release \
            -DCMAKE_CXX_FLAGS="$LL_BUILD_RELEASE" \
            -DLLSHMFRAME_BUILD_EXAMPLES=OFF \
            -DLLSHMFRAME_BUILD_TESTS=OFF \
            $(cmake_cxx_standard $LL_BUILD_RELEASE)

        cmake --build . --config Release --target llshmframe --parallel $AUTOBUILD_CPU_COUNT
        cmake --install . --config Release

        cd "$top"
        mkdir -p "$stage/LICENSES"
        cp "$top/LICENSE" "$stage/LICENSES/"

        echo "$version" > "$stage/VERSION.txt"
    ;;
    darwin*|linux*)
        # Not yet built/tested on this platform -- windows64 is the only
        # platform actually exercised by the embedded-browser project so
        # far. Follow the windows branch's pattern (plain CMake configure +
        # install into $stage, same VERSION.txt/LICENSES handling) once
        # there's a real need to build here.
        exit 1
    ;;
esac
