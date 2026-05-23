#!/usr/bin/env bash

# Save the original working directory (build directory)
BUILD_DIR="$(pwd)"

# Get script directory (where x264 source is)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Change to source directory for git operations
cd "$SCRIPT_DIR"

# Get git commit hash (short)
GIT_HASH=`git rev-list HEAD -n 1 2>/dev/null | cut -c 1-7`
[ -z "$GIT_HASH" ] && GIT_HASH="unknown"

# Read config files from build directory
BIT_DEPTH=`sed -n 's/^#define X264_BIT_DEPTH[[:space:]][[:space:]]*//p' < "$BUILD_DIR/x264_config.h" | awk '{print $1}'`
if [ "$BIT_DEPTH" = "0" ] ; then
    BIT_DEPTH="8bit+10bit"
else
    BIT_DEPTH="${BIT_DEPTH}bit"
fi
CHROMA_FORMATS=`sed -n 's/^#define X264_CHROMA_FORMAT[[:space:]][[:space:]]*//p' < "$BUILD_DIR/x264_config.h" | awk '{print $1}'`
if [ "$CHROMA_FORMATS" = "0" ] ; then
    CHROMA_FORMATS="all"
elif [ "$CHROMA_FORMATS" = "X264_CSP_I420" ] ; then
    CHROMA_FORMATS="4:2:0"
elif [ "$CHROMA_FORMATS" = "X264_CSP_I422" ] ; then
    CHROMA_FORMATS="4:2:2"
elif [ "$CHROMA_FORMATS" = "X264_CSP_I444" ] ; then
    CHROMA_FORMATS="4:4:4"
fi
BUILD_ARCH=`sed -n 's/^SYS_ARCH=//p' < "$BUILD_DIR/config.mak" | head -n 1`
BUILD_SYS=`sed -n 's/^SYS=//p' < "$BUILD_DIR/config.mak" | head -n 1`
BUILD_CC=`sed -n 's/^CC=//p' < "$BUILD_DIR/config.mak" | head -n 1`

# Get target CPU from environment variable or config.mak
TARGET_CPU="${X264_TARGET_CPU:-}"
if [ -z "$TARGET_CPU" ] && grep -q "^X264_TARGET_CPU=" "$BUILD_DIR/config.mak" 2>/dev/null; then
    TARGET_CPU=`sed -n 's/^X264_TARGET_CPU=//p' < "$BUILD_DIR/config.mak" | head -n 1`
fi

# Get build date
BUILD_DATE=`date +%Y%m%d`

# Build platform and toolchain tags
case "$BUILD_SYS" in
    WINDOWS|CYGWIN|MSYS) PLATFORM_INFO="[Windows]" ;;
    *) PLATFORM_INFO="[${BUILD_SYS}]" ;;
esac
case "$BUILD_ARCH" in
    X86_64|AARCH64) ARCH_BITS="64 bit" ;;
    *) ARCH_BITS="${BUILD_ARCH}" ;;
esac
BUILD_CC_COMMAND=${BUILD_CC%% *}
CC_VERSION=`${BUILD_CC_COMMAND:-cc} --version 2>/dev/null | head -n 1`
if echo "$CC_VERSION" | grep -qi "clang" ; then
    CC_VERSION=`printf '%s\n' "$CC_VERSION" | sed -n 's/.*clang version \([0-9][^ ]*\).*/clang \1/p'`
elif echo "$CC_VERSION" | grep -qi "gcc" ; then
    CC_VERSION=`printf '%s\n' "$CC_VERSION" | sed -n 's/.*gcc[^0-9]*\([0-9][^ ]*\).*/gcc \1/p'`
else
    GCC_VERSION=`${BUILD_CC_COMMAND:-cc} -dumpfullversion -dumpversion 2>/dev/null | head -n 1`
    if [ -n "$GCC_VERSION" ] ; then
        CC_VERSION="gcc $GCC_VERSION"
    else
        CC_VERSION="${BUILD_CC:-cc}"
    fi
fi
TOOLCHAIN_INFO="[${CC_VERSION}]"
ARCH_INFO="[${ARCH_BITS}]"

# Build CPU optimization suffix
CPU_OPT_INFO=""
if [ -n "$TARGET_CPU" ] && [ "$TARGET_CPU" != "x86-64" ]; then
    CPU_OPT_INFO="[cpu-opt=${TARGET_CPU}]"
fi

if git rev-parse --verify HEAD >/dev/null 2>&1 ; then
    BASE_REF="${X264_VERSION_BASE_REF:-refs/remotes/videolan/master}"
    BASE_COMMIT=""
    if git rev-parse --verify "$BASE_REF^{commit}" >/dev/null 2>&1 ; then
        BASE_COMMIT=`git merge-base HEAD "$BASE_REF"`
    fi

    if [ -n "$BASE_COMMIT" ] ; then
        PLAIN_VER=`git rev-list "$BASE_COMMIT" --count`
        VER_DIFF=`git rev-list "$BASE_COMMIT"..HEAD --count`
    else
        PLAIN_VER=`git rev-list HEAD --count`
        VER_DIFF=0
    fi

    echo "#define X264_REV $PLAIN_VER"
    if [ "$VER_DIFF" -ne 0 ] ; then
        VER="$PLAIN_VER+$VER_DIFF"
    else
        VER=$PLAIN_VER
    fi
    echo "#define X264_REV_DIFF $VER_DIFF"
    if ! git -c core.filemode=false diff --quiet --ignore-cr-at-eol -- . ; then
        VER="${VER}M"
    fi
    echo "#define X264_VERSION \" r$VER\""
else
    echo "#define X264_VERSION \"\""
fi

# Unified version format for both branches
API=`grep '#define X264_BUILD' < "$SCRIPT_DIR/x264.h" | sed -e 's/.* \([1-9][0-9]*\).*/\1/'`
POINTVER="${VER} ${GIT_HASH} ${PLATFORM_INFO}${TOOLCHAIN_INFO}${ARCH_INFO}${CPU_OPT_INFO} @ADE ${BIT_DEPTH}"
POINTVER_SHORT="${VER} ${GIT_HASH} @ADE"
COREVER="core $API r${VER} ${GIT_HASH} ${PLATFORM_INFO}${TOOLCHAIN_INFO}${ARCH_INFO}${CPU_OPT_INFO} @ADE ${BIT_DEPTH}"
COREVER_SHORT="core $API r${VER} ${GIT_HASH} @ADE"
echo "#define X264_POINTVER \"0.$API.$POINTVER\""
echo "#define X264_POINTVER_SHORT \"0.$API.$POINTVER_SHORT\""
echo "#define X264_COREVER \"$COREVER\""
echo "#define X264_COREVER_SHORT \"$COREVER_SHORT\""
