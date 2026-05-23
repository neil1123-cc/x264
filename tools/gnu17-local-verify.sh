#!/bin/sh
set -eu

cmd=${1:-all}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
root_diag=$(cygpath -m "$root" 2>/dev/null || printf '%s\n' "$root")
build_root=${BUILD_ROOT:-$root/build/gnu17-local-verify}
build_8b=${BUILD_8B:-$build_root/8b}
build_10b=${BUILD_10B:-$build_root/10b}
build_all=${BUILD_ALL:-$build_root/all}
config_8b=${CONFIG_8B:-$build_8b}
config_10b=${CONFIG_10B:-$build_10b}
config_all=${CONFIG_ALL:-$build_all}
tmpdir=${TMPDIR:-$root/build/tmp}
tmpdir_posix=$(cygpath -u "$tmpdir" 2>/dev/null || printf '%s\n' "$tmpdir")
tmpdir_win=$(cygpath -m "$tmpdir_posix" 2>/dev/null || printf '%s\n' "$tmpdir_posix")
make_jobs=${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '%s\n' 1)}
CONFIG_ARGS=${CONFIG_ARGS:---disable-cli}
msys2_root=${MSYS2_ROOT:-/d/msys64}
msys2_clang64=${MSYS2_CLANG64:-$msys2_root/clang64}
msys2_usr_local=${MSYS2_USR_LOCAL:-$msys2_root/usr/local}
msys2_root_diag=$(cygpath -m "$msys2_root" 2>/dev/null || printf '%s\n' "$msys2_root")
msys2_clang64_diag=$(cygpath -m "$msys2_clang64" 2>/dev/null || printf '%s\n' "$msys2_clang64")
msys2_usr_local_diag=$(cygpath -m "$msys2_usr_local" 2>/dev/null || printf '%s\n' "$msys2_usr_local")
msys2_pkg_config_path=${MSYS2_PKG_CONFIG_PATH:-$msys2_usr_local/lib/pkgconfig:$msys2_usr_local/share/pkgconfig:$msys2_clang64/lib/pkgconfig:$msys2_clang64/share/pkgconfig}

export TMP=$tmpdir_win
export TEMP=$tmpdir_win
export TMPDIR=$tmpdir_posix
export PATH=${MSYS2_PATH:-$msys2_clang64/bin:$msys2_root/usr/bin}:$PATH

mkdir -p "$build_root" "$TMPDIR"

source_root_config_backup_dir=
source_root_config_files="config.h config.mak x264_config.h"

path_exists()
{
    [ -e "$1" ] || [ -L "$1" ]
}

restore_source_root_configs()
{
    [ -n "$source_root_config_backup_dir" ] || return 0

    for source_root_config_file in $source_root_config_files; do
        source_root_config_backup=$source_root_config_backup_dir/$source_root_config_file
        source_root_config_path=$root/$source_root_config_file
        path_exists "$source_root_config_backup" || continue
        if path_exists "$source_root_config_path"; then
            printf '%s\n' "leaving isolated source-root config at $source_root_config_backup because $source_root_config_path was recreated" >&2
        else
            mv "$source_root_config_backup" "$source_root_config_path"
        fi
    done

    rmdir "$source_root_config_backup_dir" 2>/dev/null || true
    source_root_config_backup_dir=
}

cleanup()
{
    restore_source_root_configs
}

trap cleanup EXIT
trap 'status=$?; cleanup; trap - HUP INT TERM; exit "$status"' HUP INT TERM

isolate_source_root_configs_for_smoke()
{
    source_root_config_found=
    for source_root_config_file in $source_root_config_files; do
        if path_exists "$root/$source_root_config_file"; then
            source_root_config_found=yes
        fi
    done
    [ -n "$source_root_config_found" ] || return 0

    if [ -z "$source_root_config_backup_dir" ]; then
        source_root_config_backup_dir=$(mktemp -d "$build_root/source-root-config-backup.XXXXXX")
    fi

    for source_root_config_file in $source_root_config_files; do
        source_root_config_path=$root/$source_root_config_file
        path_exists "$source_root_config_path" || continue
        mv "$source_root_config_path" "$source_root_config_backup_dir/$source_root_config_file"
    done
}

run_whitespace()
{
    git -C "$root" diff -- \
        encoder/encoder.c encoder/ratecontrol.c encoder/slicetype.c x264.c autocomplete.c \
        filters/audio/internal.c output/flv_bytestream.c tools/gnu17-local-verify.sh Status.md | awk '
        /^\+[^+]/ {
            line = $0
            sub(/\r$/, "", line)
            if( line ~ /[ \t]$/ )
            {
                print "trailing whitespace in changed line: " line
                bad = 1
            }
        }
        END { exit bad }
    '
}

configure_build()
{
    label=$1
    bit_depth=$2
    extra_args=$3
    build_dir=$build_root/$label

    mkdir -p "$build_dir"
    (
        cd "$build_dir"
        PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-$msys2_pkg_config_path} \
        CC=${CC:-cc} "$root/configure" --enable-static \
            --bit-depth="$bit_depth" $CONFIG_ARGS $extra_args
        make -j"$make_jobs"
        if [ "$bit_depth" = all ]; then
            [ ! -x ./checkasm8.exe ] || ./checkasm8.exe 0
            [ ! -x ./checkasm10.exe ] || ./checkasm10.exe 0
        elif [ -x ./checkasm${bit_depth}.exe ]; then
            ./checkasm${bit_depth}.exe 0
        fi
    )
}

run_builds()
{
    configure_build 8b 8 "--chroma-format=420"
    configure_build 10b 10 "--chroma-format=420"
    configure_build all all ""
}

assert_token_present()
{
    assert_label=$1
    assert_text=$2
    assert_token=$3
    case " $assert_text " in
        *" $assert_token "*) ;;
        *)
            printf '%s\n' "missing expected token for $assert_label: $assert_token" >&2
            printf '%s\n' "$assert_label tokens: $assert_text" >&2
            exit 1
            ;;
    esac
}

assert_token_present_any()
{
    assert_label=$1
    assert_text=$2
    shift 2

    for assert_token in "$@"; do
        case " $assert_text " in
            *" $assert_token "*) return 0 ;;
        esac
    done

    printf '%s\n' "missing expected token for $assert_label: $*" >&2
    printf '%s\n' "$assert_label tokens: $assert_text" >&2
    exit 1
}

assert_no_feature_test_macro_leak()
{
    assert_label=$1
    assert_text=$2
    case " $assert_text " in
        *"_GNU_SOURCE"*|*"_POSIX_C_SOURCE"*)
            printf '%s\n' "feature-test macro leaked through $assert_label: $assert_text" >&2
            exit 1
            ;;
    esac
}

assert_first_include_flag()
{
    assert_label=$1
    assert_expected=$2
    shift 2
    for assert_flag in "$@"; do
        case "$assert_flag" in
            -I*)
                if [ "$assert_flag" != "$assert_expected" ]; then
                    printf '%s\n' "unexpected first include flag for $assert_label: got $assert_flag expected $assert_expected" >&2
                    exit 1
                fi
                return 0
                ;;
        esac
    done
    printf '%s\n' "missing include flags for $assert_label" >&2
    exit 1
}

assert_pkg_config_install_paths()
{
    assert_label=$1
    assert_sysroot=$2
    assert_cflags_text=$3
    assert_libs_text=$4
    assert_sysroot_diag=$(cygpath -m "$assert_sysroot" 2>/dev/null || printf '%s\n' "$assert_sysroot")

    assert_no_feature_test_macro_leak "$assert_label pkg-config cflags" "$assert_cflags_text"
    assert_no_feature_test_macro_leak "$assert_label pkg-config libs" "$assert_libs_text"
    assert_token_present_any "$assert_label pkg-config cflags" "$assert_cflags_text" \
        "-I$assert_sysroot/usr/local/include" "-I$assert_sysroot_diag/usr/local/include"
    assert_token_present_any "$assert_label pkg-config libs" "$assert_libs_text" \
        "-L$assert_sysroot/usr/local/lib" "-L$assert_sysroot_diag/usr/local/lib"
}

check_consumer()
{
    label=$1
    build_dir=$2
    expected_bit_depth=${3:-}
    prefix=$build_root/install-consumer/$label
    sysroot=$prefix/root
    source=$prefix/consumer.c
    binary=$prefix/consumer.exe
    strict_source=$prefix/strict-consumer.c
    strict_binary=$prefix/strict-consumer.exe

    rm -rf "$prefix"
    mkdir -p "$prefix"
    make -C "$build_dir" RANLIB=${RANLIB:-llvm-ranlib} install-lib-static DESTDIR="$sysroot"

    pkg_config_cflags=$(env -u PKG_CONFIG_PATH \
        PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1 \
        PKG_CONFIG_LIBDIR="$sysroot/usr/local/lib/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="$sysroot" \
        pkg-config --cflags x264)
    cflags=$(printf '%s\n' "$pkg_config_cflags" | sed 's/-DX264_API_IMPORTS//g')
    staged_cflags="-I$sysroot/usr/local/include"
    pkg_config_libs=$(env -u PKG_CONFIG_PATH \
        PKG_CONFIG_ALLOW_SYSTEM_LIBS=1 \
        PKG_CONFIG_LIBDIR="$sysroot/usr/local/lib/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="$sysroot" \
        pkg-config --libs x264)
    libs="$sysroot/usr/local/lib/libx264.a"
    assert_pkg_config_install_paths "$label static" "$sysroot" "$pkg_config_cflags" "$pkg_config_libs"
    assert_first_include_flag "$label static explicit include order" "$staged_cflags" $staged_cflags $cflags

    {
        printf '%s\n' '#include <stdint.h>'
        printf '%s\n' '#include <x264.h>'
        printf '%s\n' '#if !defined(X264_BIT_DEPTH) || !defined(X264_CHROMA_FORMAT)'
        printf '%s\n' '#error missing installed x264_config.h macros'
        printf '%s\n' '#endif'
        if [ -n "$expected_bit_depth" ]; then
            printf '%s\n' "#if X264_BIT_DEPTH != $expected_bit_depth"
            printf '%s\n' '#error unexpected installed bit depth'
            printf '%s\n' '#endif'
        fi
        printf '%s\n' 'int main(void)'
        printf '%s\n' '{'
        printf '%s\n' '    (void)x264_chroma_format;'
        printf '%s\n' '    return 0;'
        printf '%s\n' '}'
    } > "$source"

    cc -std=gnu17 -Wall -Wextra -Werror "$source" $staged_cflags $cflags "$libs" -o "$binary"
    "$binary"
    cc -std=gnu17 -Wall -Wextra -Werror "$source" $staged_cflags $cflags $pkg_config_libs -o "$binary.pkg-config"
    "$binary.pkg-config"

    {
        printf '%s\n' '#include <stdint.h>'
        printf '%s\n' '#include <x264.h>'
        printf '%s\n' '#if defined(_GNU_SOURCE) || defined(_POSIX_C_SOURCE)'
        printf '%s\n' '#error installed public cflags must not force feature-test macros'
        printf '%s\n' '#endif'
        printf '%s\n' '#if !defined(X264_BIT_DEPTH) || !defined(X264_CHROMA_FORMAT)'
        printf '%s\n' '#error missing installed x264_config.h macros'
        printf '%s\n' '#endif'
        printf '%s\n' 'int main(void)'
        printf '%s\n' '{'
        printf '%s\n' '    (void)x264_chroma_format;'
        printf '%s\n' '    return 0;'
        printf '%s\n' '}'
    } > "$strict_source"
    cc -std=c17 -pedantic -Wall -Wextra -Werror "$strict_source" $staged_cflags $cflags "$libs" -o "$strict_binary"
    "$strict_binary"
}

run_consumers()
{
    check_consumer 8b "$build_8b" 8
    check_consumer 10b "$build_10b" 10
    check_consumer all "$build_all"

    prefix=$build_root/cxx-consumer
    sysroot=$build_root/install-consumer/8b/root
    mkdir -p "$prefix"
    {
        printf '%s\n' '#include <stdint.h>'
        printf '%s\n' 'extern "C" {'
        printf '%s\n' '#include <x264.h>'
        printf '%s\n' '}'
        printf '%s\n' 'int main()'
        printf '%s\n' '{'
        printf '%s\n' '    (void)x264_chroma_format;'
        printf '%s\n' '    return X264_BIT_DEPTH == 8 ? 0 : 1;'
        printf '%s\n' '}'
    } > "$prefix/consumer.cpp"
    cxx_pkg_config_cflags=$(env -u PKG_CONFIG_PATH \
        PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1 \
        PKG_CONFIG_LIBDIR="$sysroot/usr/local/lib/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="$sysroot" \
        pkg-config --cflags x264)
    cxx_cflags=$(printf '%s\n' "$cxx_pkg_config_cflags" | sed 's/-DX264_API_IMPORTS//g')
    cxx_pkg_config_libs=$(env -u PKG_CONFIG_PATH \
        PKG_CONFIG_ALLOW_SYSTEM_LIBS=1 \
        PKG_CONFIG_LIBDIR="$sysroot/usr/local/lib/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="$sysroot" \
        pkg-config --libs x264)
    assert_pkg_config_install_paths "8b cxx static" "$sysroot" "$cxx_pkg_config_cflags" "$cxx_pkg_config_libs"
    assert_first_include_flag "8b cxx static explicit include order" "-I$sysroot/usr/local/include" "-I$sysroot/usr/local/include" $cxx_cflags
    c++ -std=c++17 -Wall -Wextra -Werror "$prefix/consumer.cpp" "-I$sysroot/usr/local/include" $cxx_cflags \
        "$sysroot/usr/local/lib/libx264.a" -o "$prefix/consumer.exe"
    "$prefix/consumer.exe"
}

run_warning_set()
{
    config_dir=$1
    high_bit_depth=$2
    bit_depth=$3
    includes="-I$root -I$config_dir -DHAVE_CONFIG_H -DHIGH_BIT_DEPTH=$high_bit_depth -DBIT_DEPTH=$bit_depth"
    flags="-std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wno-unused-parameter"
    for f in common/macroblock.c common/mvpred.c common/pixel.c common/cabac.c common/frame.c \
             encoder/encoder.c encoder/analyse.c encoder/me.c encoder/ratecontrol.c; do
        cc $flags $includes -c "$root/$f" -o /dev/null
    done
    cc $flags -Wno-cast-function-type-mismatch $includes -c "$root/tools/checkasm.c" -o /dev/null
}

run_warnings()
{
    run_warning_set "$config_8b" 0 8
    run_warning_set "$config_10b" 1 10
    run_warning_set "$config_all" 0 8
    run_warning_set "$config_all" 1 10
}

run_warning_extra_compile_set()
{
    config_label=$1
    config_dir=$2
    high_bit_depth=$3
    bit_depth=$4
    extra_flags=$5
    shift 5
    includes="-I$root -I$config_dir -DHAVE_CONFIG_H -DHIGH_BIT_DEPTH=$high_bit_depth -DBIT_DEPTH=$bit_depth"
    flags="-std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Wformat=2 -Wshadow -Wno-unused-parameter $extra_flags"
    for f in "$@"; do
        printf 'warning-extra-source: config=%s bit_depth=%s file=%s\n' "$config_label" "$bit_depth" "$f"
        cc $flags $includes -c "$root/$f" -o /dev/null
    done
}

summarize_warning_extra()
{
    log=$1
    summary=$2
    name=$3
    status=$4
    mode=${5:-default}
    awk -v name="$name" -v status="$status" -v mode="$mode" -v root="$root" -v root_diag="$root_diag" \
        -v msys2_root="$msys2_root" -v msys2_clang64="$msys2_clang64" -v msys2_usr_local="$msys2_usr_local" \
        -v msys2_root_diag="$msys2_root_diag" -v msys2_clang64_diag="$msys2_clang64_diag" -v msys2_usr_local_diag="$msys2_usr_local_diag" '
        function classify(file) {
            if( mode != "external" )
                return "none"
            gsub(/\\\\/, "/", file)
            if( index(file, root "/") == 1 || index(file, root_diag "/") == 1 )
                return "self"
            if( index(file, msys2_usr_local "/include/") == 1 || index(file, msys2_clang64 "/include/") == 1 || index(file, msys2_root "/") == 1 ||
                index(file, msys2_usr_local_diag "/include/") == 1 || index(file, msys2_clang64_diag "/include/") == 1 || index(file, msys2_root_diag "/") == 1 )
                return "external"
            return "other"
        }
        function record(kind) {
            file = $1
            site = $1
            sub(/:[0-9]+:.*/, "", file)
            counts[file]++
            site_counts[kind " " site]++
            if( kind == "warnings" )
            {
                warning_option = "warning-without-option"
                if( match($0, /\[-W[^][]+\]/) )
                    warning_option = substr($0, RSTART + 1, RLENGTH - 2)
                warning_options[warning_option]++
            }
            if( current_file != "" )
            {
                source_counts[current_source]++
                input_counts[current_file]++
                if( kind == "warnings" )
                    source_warnings[current_source]++
                else if( kind == "errors" )
                    source_errors[current_source]++
                else if( kind == "fatal" )
                    source_fatal[current_source]++
            }
            bucket = classify(file)
            if( bucket != "none" )
            {
                bucket_counts[bucket ":" kind]++
                bucket_reported_counts[bucket " " file]++
                if( current_file != "" )
                    bucket_input_counts[bucket " " current_file]++
            }
        }
        /^warning-extra-source: / {
            source = $0
            sub(/^warning-extra-source: /, "", source)
            current_source = source
            current_file = source
            sub(/^.*file=/, "", current_file)
            sources[current_source] = 1
            next
        }
        /fatal error:|fatal:/ {
            fatal++
            record("fatal")
            next
        }
        /error:/ {
            errors++
            record("errors")
            next
        }
        /warning:/ {
            warnings++
            record("warnings")
        }
        END {
            printf("diagnostic: %s\n", name)
            printf("status: %d\n", status + 0)
            printf("warnings: %d\n", warnings + 0)
            printf("errors: %d\n", errors + 0)
            printf("fatal: %d\n", fatal + 0)
            if( mode == "external" )
            {
                for( i = 1; i <= 3; i++ )
                {
                    bucket = i == 1 ? "self" : i == 2 ? "external" : "other"
                    printf("bucket:%s warnings=%d errors=%d fatal=%d\n", bucket,
                           bucket_counts[bucket ":warnings"] + 0,
                           bucket_counts[bucket ":errors"] + 0,
                           bucket_counts[bucket ":fatal"] + 0)
                }
            }
            print ""
            print "by_source:"
            for( source in sources )
                if( source_counts[source] > 0 )
                    printf("%d warnings=%d errors=%d fatal=%d %s\n", source_counts[source], source_warnings[source] + 0, source_errors[source] + 0, source_fatal[source] + 0, source) | "sort -rn"
            close("sort -rn")
            print ""
            print "by_reported_file:"
            for( file in counts )
                printf("%d %s\n", counts[file], file) | "sort -rn"
            close("sort -rn")
            print ""
            print "by_warning_option:"
            for( warning_option in warning_options )
                printf("%d %s\n", warning_options[warning_option], warning_option) | "sort -rn"
            close("sort -rn")
            print ""
            print "by_diagnostic_site:"
            for( site in site_counts )
                printf("%d %s\n", site_counts[site], site) | "sort -rn"
            close("sort -rn")
            print ""
            if( mode == "external" )
            {
                print "by_bucket_reported_file:"
                for( file in bucket_reported_counts )
                    printf("%d %s\n", bucket_reported_counts[file], file) | "sort -rn"
                close("sort -rn")
                print ""
                print "by_bucket_input_file:"
                for( file in bucket_input_counts )
                    printf("%d %s\n", bucket_input_counts[file], file) | "sort -rn"
                close("sort -rn")
                print ""
            }
            print ""
            print "bucket_by_input_file:"
            for( file in input_counts )
                printf("%d %s\n", input_counts[file], file) | "sort -rn"
            close("sort -rn")
        }
    ' "$log" > "$summary"
}

run_warning_extra_diagnostic()
{
    name=$1
    extra_flags=$2
    shift 2
    out_dir=$build_root/warnings-extra
    log=$out_dir/$name.log
    summary=$out_dir/$name.summary

    mkdir -p "$out_dir"
    : > "$log"
    status=0
    run_warning_extra_compile_set 8b "$config_8b" 0 8 "$extra_flags" "$@" >>"$log" 2>&1 || status=$?
    run_warning_extra_compile_set 10b "$config_10b" 1 10 "$extra_flags" "$@" >>"$log" 2>&1 || status=$?
    run_warning_extra_compile_set all-8b "$config_all" 0 8 "$extra_flags" "$@" >>"$log" 2>&1 || status=$?
    run_warning_extra_compile_set all-10b "$config_all" 1 10 "$extra_flags" "$@" >>"$log" 2>&1 || status=$?
    summarize_warning_extra "$log" "$summary" "$name" "$status" "$name"
    warnings=$(awk '/^warnings:/ { print $2 }' "$summary")
    errors=$(awk '/^errors:/ { print $2 }' "$summary")
    fatal=$(awk '/^fatal:/ { print $2 }' "$summary")
    printf '%s\n' "warnings-extra:$name recorded $warnings warnings, $errors errors, $fatal fatal diagnostics in $log"
    printf '%s\n' "warnings-extra:$name summary in $summary"
    if [ "$status" -ne 0 ]; then
        printf '%s\n' "warnings-extra:$name compiler status $status (diagnostic only)"
    fi
}

run_warnings_extra_double()
{
    run_warning_extra_diagnostic double "-Wdouble-promotion" \
        encoder/encoder.c encoder/ratecontrol.c encoder/analyse.c
}

run_warnings_extra_conversion()
{
    run_warning_extra_diagnostic conversion "-Wconversion -Wsign-conversion" \
        output/gop.c output/flv_bytestream.c encoder/ratecontrol.c
}

run_warnings_extra_external()
{
    run_warning_extra_diagnostic external "-Wconversion -Wsign-conversion -I$msys2_usr_local/include -I$msys2_clang64/include" \
        x264.c autocomplete.c input/lavf.c input/ffms.c input/audio/lavf.c input/audio/lsmash.c \
        output/mp4_lsmash.c audio/encoders.c filters/audio/audio_filters.c filters/audio/internal.c
}

summarize_warning_extra_compare()
{
    name=$1
    summary=$2
    awk -v name="$name" '
        /^warnings:/ { warnings = $2 }
        /^errors:/ { errors = $2 }
        /^fatal:/ { fatal = $2 }
        /^bucket:/ { external_buckets[++external_bucket_count] = $0 }
        /^by_source:/ { section = "source"; next }
        /^by_reported_file:/ { section = "reported"; next }
        /^by_warning_option:/ { section = "warning_option"; next }
        /^by_diagnostic_site:/ { section = "diagnostic_site"; next }
        /^by_bucket_reported_file:/ { section = "bucket_reported"; next }
        /^by_bucket_input_file:/ { section = "bucket_input"; next }
        /^bucket_by_input_file:/ { section = "input"; next }
        /^$/ { next }
        section == "source" && source_count < 3 {
            source_count++
            source[source_count] = $0
            next
        }
        section == "input" && input_count < 3 {
            input_count++
            input[input_count] = $0
            next
        }
        section == "warning_option" && warning_option_count < 5 {
            warning_option_count++
            warning_option[warning_option_count] = $0
            next
        }
        section == "diagnostic_site" && diagnostic_site_count < 5 {
            diagnostic_site_count++
            diagnostic_site[diagnostic_site_count] = $0
            next
        }
        section == "bucket_reported" && bucket_reported_count < 6 {
            bucket_reported_count++
            bucket_reported[bucket_reported_count] = $0
            next
        }
        section == "bucket_input" && bucket_input_count < 6 {
            bucket_input_count++
            bucket_input[bucket_input_count] = $0
            next
        }
        END {
            printf("%s warnings=%d errors=%d fatal=%d\n", name, warnings + 0, errors + 0, fatal + 0)
            for( i = 1; i <= external_bucket_count; i++ )
                print external_buckets[i]
            for( i = 1; i <= warning_option_count; i++ )
                printf("  warning[%d] %s\n", i, warning_option[i])
            for( i = 1; i <= diagnostic_site_count; i++ )
                printf("  site[%d] %s\n", i, diagnostic_site[i])
            for( i = 1; i <= bucket_reported_count; i++ )
                printf("  bucket-file[%d] %s\n", i, bucket_reported[i])
            for( i = 1; i <= bucket_input_count; i++ )
                printf("  bucket-input[%d] %s\n", i, bucket_input[i])
            for( i = 1; i <= source_count; i++ )
                printf("  source[%d] %s\n", i, source[i])
            for( i = 1; i <= input_count; i++ )
                printf("  input[%d] %s\n", i, input[i])
        }
    ' "$summary"
}

run_warnings_extra_compare()
{
    out_dir=$build_root/warnings-extra
    for name in double conversion; do
        summary=$out_dir/$name.summary
        if [ ! -f "$summary" ]; then
            printf '%s\n' "missing warnings-extra:$name summary: $summary" >&2
            exit 1
        fi
        summarize_warning_extra_compare "$name" "$summary"
    done
    summary=$out_dir/external.summary
    if [ -f "$summary" ]; then
        summarize_warning_extra_compare external "$summary"
    else
        printf '%s\n' "external summary not present (run warnings-extra:external to include it)"
    fi
    probe_dir=$build_root/feature-probes
    for name in gnu17 c17; do
        summary=$probe_dir/$name.summary
        if [ -f "$summary" ]; then
            awk -v name="$name" '
                /^status:/ { status = $2 }
                /^std:/ { std = $2 }
                END { printf("feature-probe:%s status=%d std=%s\n", name, status + 0, std) }
            ' "$summary"
        else
            printf '%s\n' "feature-probe:$name summary not present (run feature-probe to include it)"
        fi
    done
}

run_warnings_extra_baseline_save()
{
    out_dir=$build_root/warnings-extra
    baseline_dir=${WARNINGS_EXTRA_BASELINE_DIR:-$build_root/warnings-extra-baseline}
    mkdir -p "$baseline_dir"

    copied=0
    for name in double conversion external; do
        summary=$out_dir/$name.summary
        if [ -f "$summary" ]; then
            cp "$summary" "$baseline_dir/$name.summary"
            copied=$((copied + 1))
            printf '%s\n' "warnings-extra:baseline saved $name -> $baseline_dir/$name.summary"
        else
            printf '%s\n' "warnings-extra:baseline skipped missing $summary"
        fi
    done
    if [ "$copied" -eq 0 ]; then
        printf '%s\n' "warnings-extra:baseline found no summaries; run warnings-extra:* first" >&2
        exit 1
    fi
}

run_warnings_extra_delta()
{
    out_dir=$build_root/warnings-extra
    baseline_dir=${WARNINGS_EXTRA_BASELINE_DIR:-$build_root/warnings-extra-baseline}

    for name in double conversion external; do
        baseline=$baseline_dir/$name.summary
        summary=$out_dir/$name.summary
        if [ -f "$baseline" ] && [ -f "$summary" ]; then
            printf '%s\n' "warnings-extra:delta $name"
            diff -u "$baseline" "$summary" || true
        elif [ ! -f "$baseline" ]; then
            printf '%s\n' "warnings-extra:delta missing baseline for $name: $baseline"
        else
            printf '%s\n' "warnings-extra:delta missing current summary for $name: $summary"
        fi
    done
}

run_warnings_extra()
{
    run_warnings_extra_double
    run_warnings_extra_conversion
}

run_feature_probe_one()
{
    name=$1
    std=$2
    out_dir=$build_root/feature-probes
    source=$out_dir/$name.c
    log=$out_dir/$name.log
    summary=$out_dir/$name.summary

    mkdir -p "$out_dir"
    {
        printf '%s\n' '#include <stddef.h>'
        printf '%s\n' '#include <stdalign.h>'
        printf '%s\n' '_Static_assert(__STDC_VERSION__ >= 201710L, "C17 semantics required");'
        printf '%s\n' 'struct probe { char c; alignas(16) int x; };'
        printf '%s\n' '_Static_assert(alignof(struct probe) >= 16, "alignof must observe alignas");'
        printf '%s\n' '_Static_assert(offsetof(struct probe, x) % alignof(int) == 0, "offsetof must be constant");'
        printf '%s\n' 'int main(void) { return 0; }'
    } > "$source"

    status=0
    ${CC:-cc} -std="$std" -D_GNU_SOURCE -Wall -Wextra -Werror "$source" -o "$out_dir/$name.exe" >"$log" 2>&1 || status=$?
    {
        printf '%s\n' "feature-probe:$name"
        printf '%s\n' "status: $status"
        printf '%s\n' "compiler: ${CC:-cc}"
        printf '%s\n' "std: $std"
        printf '%s\n' "log: $log"
        printf '%s\n' "exe: $out_dir/$name.exe"
        printf '%s\n' 'checks: _Static_assert alignas alignof offsetof'
    } > "$summary"
    if [ "$status" -eq 0 ]; then
        printf '%s\n' "feature-probe:$name ok"
    else
        printf '%s\n' "feature-probe:$name fail status=$status log=$log" >&2
        return "$status"
    fi
}

run_feature_probe()
{
    run_feature_probe_one gnu17 gnu17
    run_feature_probe_one c17 c17
    printf '%s\n' "feature-probe summaries in $build_root/feature-probes"
}

run_avi_legacy_probe()
{
    out_dir=$build_root/avi-legacy-probe
    source=$out_dir/probe.c
    log=$out_dir/probe.log
    summary=$out_dir/probe.summary
    mkdir -p "$out_dir"

    avi_cflags=
    avi_libs=-lavformat
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libavformat 2>/dev/null; then
        avi_cflags=$(pkg-config --cflags libavformat)
        avi_libs=$(pkg-config --libs libavformat)
    fi

    {
        printf '%s\n' '#include <stddef.h>'
        printf '%s\n' '#include <libavformat/avformat.h>'
        printf '%s\n' '_Static_assert(sizeof(((AVFormatContext *)0)->filename) > 0, "AVFormatContext.filename is required");'
        printf '%s\n' '_Static_assert(sizeof(((AVStream *)0)->codec) > 0, "AVStream.codec is required");'
        printf '%s\n' 'int main(void)'
        printf '%s\n' '{'
        printf '%s\n' '    (void)&av_register_all;'
        printf '%s\n' '    (void)&av_guess_format;'
        printf '%s\n' '    return 0;'
        printf '%s\n' '}'
    } > "$source"

    status=0
    ${CC:-cc} -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Werror $avi_cflags -c "$source" -o "$out_dir/probe.o" >"$log" 2>&1 || status=$?
    {
        printf '%s\n' 'avi-legacy-probe'
        printf '%s\n' "status: $status"
        printf '%s\n' "compiler: ${CC:-cc}"
        printf '%s\n' "cflags: $avi_cflags"
        printf '%s\n' "libs: $avi_libs"
        printf '%s\n' "log: $log"
        printf '%s\n' 'checks: av_register_all av_guess_format AVFormatContext.filename AVStream.codec'
    } > "$summary"

    if [ "$status" -eq 0 ]; then
        printf '%s\n' "avi-legacy-probe ok: legacy libavformat AVI API is available"
    else
        printf '%s\n' "avi-legacy-probe unavailable status=$status summary=$summary"
        printf '%s\n' "AVI output uses legacy libavformat API; this diagnostic failure is not a core GNU17 failure."
    fi
}

configure_smoke_cli()
{
    label=$1
    extra_args=$2
    smoke_dir=$build_root/$label
    mkdir -p "$smoke_dir"
    isolate_source_root_configs_for_smoke
    (
        cd "$smoke_dir"
        PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-$msys2_pkg_config_path} \
        CC=${CC:-cc} RC= "$root/configure" --enable-static --chroma-format=420 --bit-depth=8 $extra_args
        make -j"$make_jobs" x264
    ) || {
        printf '%s\n' "smoke configure/build failed: label=$label dir=$smoke_dir" >&2
        printf '%s\n' "CONFIG_ARGS=$extra_args" >&2
        printf '%s\n' "PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-$msys2_pkg_config_path}" >&2
        printf '%s\n' "TMP=$TMP TEMP=$TEMP TMPDIR=$TMPDIR" >&2
        exit 1
    }
    exe=$(awk -F= '/^EXE=/ { print $2 }' "$smoke_dir/config.mak")
    smoke_bin=$smoke_dir/x264$exe
    if [ ! -x "$smoke_bin" ]; then
        printf '%s\n' "missing smoke x264 binary: $smoke_bin" >&2
        exit 1
    fi
}

write_smoke_y4m()
{
    write_smoke_y4m_frames "$1" 1 25:1
}

write_smoke_y4m_frames()
{
    smoke_y4m_fps=$3
    write_smoke_y4m_with_header "$1" "$2" "YUV4MPEG2 W16 H16 F$smoke_y4m_fps Ip A1:1 C420"
}

write_smoke_y4m_with_header()
{
    smoke_y4m_path=$1
    smoke_y4m_frames=$2
    smoke_y4m_header=$3
    {
        printf '%s\n' "$smoke_y4m_header"
        smoke_y4m_i=0
        while [ "$smoke_y4m_i" -lt "$smoke_y4m_frames" ]; do
            printf '%s\n' 'FRAME'
            dd if=/dev/zero bs=384 count=1 2>/dev/null
            smoke_y4m_i=$((smoke_y4m_i + 1))
        done
    } > "$smoke_y4m_path"
}

require_ffprobe()
{
    ffprobe_label=$1
    command -v ffprobe >/dev/null 2>&1 || { printf '%s\n' "ffprobe is required for $ffprobe_label external dependency smoke; this is not a core GNU17 failure. Install ffprobe or adjust PATH." >&2; exit 1; }
}

require_pkg_config_package()
{
    pkg_label=$1
    pkg_name=$2
    pkg_path=${PKG_CONFIG_PATH:-$msys2_pkg_config_path}

    command -v pkg-config >/dev/null 2>&1 ||
    {
        printf '%s\n' "pkg-config is required for $pkg_label external dependency smoke; this is not a core GNU17 failure. Install pkg-config or adjust PATH." >&2
        exit 1
    }
    PKG_CONFIG_PATH=$pkg_path pkg-config --exists "$pkg_name" 2>/dev/null &&
    PKG_CONFIG_PATH=$pkg_path pkg-config --libs --static "$pkg_name" >/dev/null 2>&1 ||
    {
        printf '%s\n' "$pkg_name pkg-config metadata is required for $pkg_label external dependency smoke; this is not a core GNU17 failure." >&2
        printf '%s\n' "PKG_CONFIG_PATH=$pkg_path" >&2
        exit 1
    }
}

assert_duration_between()
{
    duration_label=$1
    duration_value=$2
    duration_min=$3
    duration_max=$4
    awk -v label="$duration_label" -v value="$duration_value" -v min="$duration_min" -v max="$duration_max" '
        BEGIN {
            duration = value + 0.0
            if( value == "" || duration < min || duration > max )
            {
                printf("unexpected %s duration: %s (expected %.6f..%.6f)\n", label, value, min, max) > "/dev/stderr"
                exit 1
            }
        }
    '
}

run_param_list_guard_smoke()
{
    guard_source=$smoke_dir/param-list-guard.c
    guard_binary=$smoke_dir/param-list-guard$exe
    guard_log=$smoke_dir/param-list-guard.log
    guard_ldflags=$(awk -F= '/^LDFLAGS=/ { print $2 }' "$smoke_dir/config.mak")
    guard_ldflagscli=$(awk -F= '/^LDFLAGSCLI=/ { print $2 }' "$smoke_dir/config.mak")

    cat > "$guard_source" <<'GUARD_C'
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x264.h"

struct guard_allocation
{
    char *base;
    size_t size;
};

#if defined(_WIN32)
#include <windows.h>

static char *make_guarded_string( const char *text, struct guard_allocation *allocation )
{
    SYSTEM_INFO info;
    DWORD old_protect;
    size_t len = strlen( text ) + 1;

    GetSystemInfo( &info );
    size_t page_size = info.dwPageSize;
    if( len > page_size )
        return NULL;
    char *mem = VirtualAlloc( NULL, page_size * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    if( !mem )
        return NULL;
    if( !VirtualProtect( mem + page_size, page_size, PAGE_NOACCESS, &old_protect ) )
    {
        VirtualFree( mem, 0, MEM_RELEASE );
        return NULL;
    }

    allocation->base = mem;
    allocation->size = page_size * 2;
    char *value = mem + page_size - len;
    memcpy( value, text, len );
    return value;
}

static void free_guarded_string( struct guard_allocation *allocation )
{
    VirtualFree( allocation->base, 0, MEM_RELEASE );
}

#else
#include <sys/mman.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

static char *make_guarded_string( const char *text, struct guard_allocation *allocation )
{
    long page_size_long = sysconf( _SC_PAGESIZE );
    if( page_size_long <= 0 )
        return NULL;
    size_t page_size = (size_t)page_size_long;
    size_t len = strlen( text ) + 1;
    if( len > page_size )
        return NULL;
    char *mem = mmap( NULL, page_size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    if( mem == MAP_FAILED )
        return NULL;
    if( mprotect( mem + page_size, page_size, PROT_NONE ) )
    {
        munmap( mem, page_size * 2 );
        return NULL;
    }

    allocation->base = mem;
    allocation->size = page_size * 2;
    char *value = mem + page_size - len;
    memcpy( value, text, len );
    return value;
}

static void free_guarded_string( struct guard_allocation *allocation )
{
    munmap( allocation->base, allocation->size );
}
#endif

static int parse_guarded_value( const char *name, const char *text )
{
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( text, &allocation );
    if( !value )
        return -2;

    x264_param_default( &param );
    int ret = x264_param_parse( &param, name, value );
    free_guarded_string( &allocation );
    return ret;
}

int main( void )
{
    static const struct
    {
        const char *name;
        const char *value;
    } cases[] = {
        { "psy-rd", "1.0" },
        { "aq3-strength", "0.7" },
        { "aq3-ifactor", "1.1" },
        { "aq3-pfactor", "1.2" },
        { "aq3-bfactor", "1.3" },
        { "deblock", "1" },
        { "qpmin", "3" },
        { "qpmax", "42" },
        { "partitions", "p8x8,p4x4,b8x8,i8x8,i4x4" },
        { "analyse", "none" },
    };

    for( size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++ )
    {
        int ret = parse_guarded_value( cases[i].name, cases[i].value );
        if( ret )
        {
            fprintf( stderr, "guarded parse failed for %s=%s: %d\n", cases[i].name, cases[i].value, ret );
            return 1;
        }
    }

    return 0;
}
GUARD_C

    if ! ${CC:-cc} -std=gnu17 -D_GNU_SOURCE -Wall -Wextra -Werror -I"$smoke_dir" -I"$root" \
        "$guard_source" "$smoke_dir/libx264.a" $guard_ldflagscli $guard_ldflags -o "$guard_binary" >"$guard_log" 2>&1; then
        printf '%s\n' "failed to build param-list guard smoke: $guard_log" >&2
        exit 1
    fi
    if ! "$guard_binary" >>"$guard_log" 2>&1; then
        printf '%s\n' "param-list guard smoke failed: $guard_log" >&2
        exit 1
    fi
}

run_smoke_cli()
{
    configure_smoke_cli smoke-cli "--disable-lavf --disable-ffms --disable-lsmash --disable-audio"
    y4m=$smoke_dir/smoke.y4m
    out=$smoke_dir/smoke.264
    qp_none=$smoke_dir/qp-none.qp
    qp_none_out=$smoke_dir/qp-none.264
    qp_trailing=$smoke_dir/qp-none-trailing.qp
    qp_trailing_out=$smoke_dir/qp-none-trailing.264
    qp_trailing_log=$smoke_dir/qp-none-trailing.log
    opts_custom_out=$smoke_dir/opts-custom.264
    float_params_out=$smoke_dir/float-params.264
    float_single_params_out=$smoke_dir/float-single-params.264
    y4m_unknown=$smoke_dir/smoke-unknown-ratio.y4m
    y4m_unknown_out=$smoke_dir/smoke-unknown-ratio.264
    y4m_420jpeg=$smoke_dir/smoke-420jpeg.y4m
    y4m_420jpeg_out=$smoke_dir/smoke-420jpeg.264
    y4m_bad_fps=$smoke_dir/smoke-bad-fps.y4m
    y4m_bad_fps_out=$smoke_dir/smoke-bad-fps.264
    y4m_bad_fps_log=$smoke_dir/smoke-bad-fps.log
    y4m_bad_csp_suffix=$smoke_dir/smoke-bad-csp-suffix.y4m
    y4m_bad_csp_suffix_out=$smoke_dir/smoke-bad-csp-suffix.264
    y4m_bad_csp_suffix_log=$smoke_dir/smoke-bad-csp-suffix.log
    y4m_bad_width=$smoke_dir/smoke-bad-width.y4m
    y4m_bad_width_out=$smoke_dir/smoke-bad-width.264
    y4m_bad_width_log=$smoke_dir/smoke-bad-width.log
    y4m_bad_height=$smoke_dir/smoke-bad-height.y4m
    y4m_bad_height_out=$smoke_dir/smoke-bad-height.264
    y4m_bad_height_log=$smoke_dir/smoke-bad-height.log
    y4m_bad_length=$smoke_dir/smoke-bad-length.y4m
    y4m_bad_length_out=$smoke_dir/smoke-bad-length.264
    y4m_bad_length_log=$smoke_dir/smoke-bad-length.log
    y4m_bad_color_range=$smoke_dir/smoke-bad-color-range.y4m
    y4m_bad_color_range_out=$smoke_dir/smoke-bad-color-range.264
    y4m_bad_color_range_log=$smoke_dir/smoke-bad-color-range.log
    raw=$smoke_dir/smoke.raw
    raw_out=$smoke_dir/smoke-raw.264
    raw_bad_res_out=$smoke_dir/smoke-raw-bad-res.264
    raw_bad_res_log=$smoke_dir/smoke-raw-bad-res.log
    param_bad_fps_out=$smoke_dir/smoke-param-bad-fps.264
    param_bad_fps_log=$smoke_dir/smoke-param-bad-fps.log
    param_partitions_out=$smoke_dir/smoke-param-partitions.264
    param_bad_partitions_out=$smoke_dir/smoke-param-bad-partitions.264
    param_bad_partitions_log=$smoke_dir/smoke-param-bad-partitions.log
    param_bad_analyse_out=$smoke_dir/smoke-param-bad-analyse.264
    param_bad_analyse_log=$smoke_dir/smoke-param-bad-analyse.log
    param_bad_deblock_out=$smoke_dir/smoke-param-bad-deblock.264
    param_bad_deblock_log=$smoke_dir/smoke-param-bad-deblock.log
    param_bad_qpmin_out=$smoke_dir/smoke-param-bad-qpmin.264
    param_bad_qpmin_log=$smoke_dir/smoke-param-bad-qpmin.log
    param_ratetol_inf_out=$smoke_dir/smoke-param-ratetol-inf.264
    param_bad_ratetol_out=$smoke_dir/smoke-param-bad-ratetol.264
    param_bad_ratetol_log=$smoke_dir/smoke-param-bad-ratetol.log
    param_keyint_inf_out=$smoke_dir/smoke-param-keyint-infinite.264
    param_bad_keyint_out=$smoke_dir/smoke-param-bad-keyint.264
    param_bad_keyint_log=$smoke_dir/smoke-param-bad-keyint.log
    param_bad_mastering_out=$smoke_dir/smoke-param-bad-mastering.264
    param_bad_mastering_log=$smoke_dir/smoke-param-bad-mastering.log
    param_bad_aq3_boundary_out=$smoke_dir/smoke-param-bad-aq3-boundary.264
    param_bad_aq3_boundary_log=$smoke_dir/smoke-param-bad-aq3-boundary.log
    param_bad_psyrd_out=$smoke_dir/smoke-param-bad-psy-rd.264
    param_bad_psyrd_log=$smoke_dir/smoke-param-bad-psy-rd.log
    param_bad_aq3_strength_out=$smoke_dir/smoke-param-bad-aq3-strength.264
    param_bad_aq3_strength_log=$smoke_dir/smoke-param-bad-aq3-strength.log
    param_bad_aq3_ifactor_out=$smoke_dir/smoke-param-bad-aq3-ifactor.264
    param_bad_aq3_ifactor_log=$smoke_dir/smoke-param-bad-aq3-ifactor.log
    param_bad_cqm4_out=$smoke_dir/smoke-param-bad-cqm4.264
    param_bad_cqm4_log=$smoke_dir/smoke-param-bad-cqm4.log
    filter_bad_pad_out=$smoke_dir/smoke-filter-bad-pad.264
    filter_bad_pad_log=$smoke_dir/smoke-filter-bad-pad.log
    cqm_flat_out=$smoke_dir/smoke-cqm-flat.264
    cqm_jvt_out=$smoke_dir/smoke-cqm-jvt.264
    cqm_missing_flat=$smoke_dir/smoke-flat-cqm-missing.cfg
    cqm_missing_flat_out=$smoke_dir/smoke-flat-cqm-missing.264
    cqm_missing_flat_log=$smoke_dir/smoke-flat-cqm-missing.log
    cqmfile_anchored=$smoke_dir/smoke-cqmfile-anchored.cfg
    cqmfile_anchored_out=$smoke_dir/smoke-cqmfile-anchored.264
    cqmfile_bad=$smoke_dir/smoke-bad-cqmfile.cfg
    cqmfile_bad_out=$smoke_dir/smoke-bad-cqmfile.264
    cqmfile_bad_log=$smoke_dir/smoke-bad-cqmfile.log
    tc_bad_header=$smoke_dir/smoke-bad-header.tc
    tc_bad_header_out=$smoke_dir/smoke-bad-header.264
    tc_bad_header_log=$smoke_dir/smoke-bad-header.log
    tc_bad_tdecimate=$smoke_dir/smoke-bad-tdecimate.tc
    tc_bad_tdecimate_out=$smoke_dir/smoke-bad-tdecimate.264
    tc_bad_tdecimate_log=$smoke_dir/smoke-bad-tdecimate.log
    stats_y4m=$smoke_dir/twopass.y4m
    stats_valid=$smoke_dir/twopass.stats
    stats_pass1_out=$smoke_dir/twopass-pass1.264
    stats_bad_timebase=$smoke_dir/twopass-bad-timebase.stats
    stats_bad_timebase_out=$smoke_dir/twopass-bad-timebase.264
    stats_bad_timebase_log=$smoke_dir/twopass-bad-timebase.log
    stats_bad_main=$smoke_dir/twopass-bad-main.stats
    stats_bad_main_out=$smoke_dir/twopass-bad-main.264
    stats_bad_main_log=$smoke_dir/twopass-bad-main.log
    stats_bad_main_ref=$smoke_dir/twopass-bad-main-ref.stats
    stats_bad_main_ref_out=$smoke_dir/twopass-bad-main-ref.264
    stats_bad_main_ref_log=$smoke_dir/twopass-bad-main-ref.log
    stats_bad_ref=$smoke_dir/twopass-bad-ref.stats
    stats_bad_ref_out=$smoke_dir/twopass-bad-ref.264
    stats_bad_ref_log=$smoke_dir/twopass-bad-ref.log
    stats_weight=$smoke_dir/twopass-weight.stats
    stats_weight_out=$smoke_dir/twopass-weight.264
    stats_weight_spaces=$smoke_dir/twopass-weight-spaces.stats
    stats_weight_spaces_out=$smoke_dir/twopass-weight-spaces.264
    stats_weight_chroma_spaces=$smoke_dir/twopass-weight-chroma-spaces.stats
    stats_weight_chroma_spaces_out=$smoke_dir/twopass-weight-chroma-spaces.264
    stats_bad_weight=$smoke_dir/twopass-bad-weight.stats
    stats_bad_weight_out=$smoke_dir/twopass-bad-weight.264
    stats_bad_weight_log=$smoke_dir/twopass-bad-weight.log
    stats_bad_bframes=$smoke_dir/twopass-bad-bframes.stats
    stats_bad_bframes_out=$smoke_dir/twopass-bad-bframes.264
    stats_bad_bframes_log=$smoke_dir/twopass-bad-bframes.log
    stats_bad_bframes_ws=$smoke_dir/twopass-bad-bframes-ws.stats
    stats_bad_bframes_ws_out=$smoke_dir/twopass-bad-bframes-ws.264
    stats_bad_bframes_ws_log=$smoke_dir/twopass-bad-bframes-ws.log
    stats_bad_lookahead_ws=$smoke_dir/twopass-bad-lookahead-ws.stats
    stats_bad_lookahead_ws_out=$smoke_dir/twopass-bad-lookahead-ws.264
    stats_bad_lookahead_ws_log=$smoke_dir/twopass-bad-lookahead-ws.log
    zone_nan_out=$smoke_dir/zone-nan.264
    zone_nan_log=$smoke_dir/zone-nan.log
    write_smoke_y4m "$y4m"
    "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$out" "$y4m" >/dev/null
    [ -s "$out" ] || { printf '%s\n' "missing CLI smoke output: $out (input: $y4m)" >&2; exit 1; }
    "$smoke_bin" --demuxer y4m --frames 1 --opts preinfo:gnu17-pre --opts postinfo:gnu17-post --opts preopt:gnu17-preopt --opts postopt:gnu17-postopt --crf 30 -o "$opts_custom_out" "$y4m" >/dev/null
    [ -s "$opts_custom_out" ] || { printf '%s\n' "missing custom opts SEI smoke output: $opts_custom_out (input: $y4m)" >&2; exit 1; }
    "$smoke_bin" --demuxer y4m --frames 1 --psy-rd '1.0|0.2' --aq3-mode 1 \
        --aq3-strength 0.1:0.2:0.3:0.4:0.5:0.6:0.7:0.8 \
        --aq3-ifactor 1.1,1.2 --aq3-pfactor 1.3 --aq3-bfactor 1.4:1.5 \
        --crf 30 -o "$float_params_out" "$y4m" >/dev/null
    [ -s "$float_params_out" ] || { printf '%s\n' "missing strict float parameter smoke output: $float_params_out (input: $y4m)" >&2; exit 1; }
    "$smoke_bin" --demuxer y4m --frames 1 --psy-rd 1.0 --aq3-mode 1 \
        --aq3-strength 0.7 --aq3-ifactor 1.1 --aq3-pfactor 1.2 --aq3-bfactor 1.3 \
        --crf 30 -o "$float_single_params_out" "$y4m" >/dev/null
    [ -s "$float_single_params_out" ] || { printf '%s\n' "missing single-value float parameter smoke output: $float_single_params_out (input: $y4m)" >&2; exit 1; }
    run_param_list_guard_smoke
    dd if=/dev/zero of="$raw" bs=384 count=1 2>/dev/null
    "$smoke_bin" --demuxer raw --input-res 16x16 --fps 25 --frames 1 --crf 30 -o "$raw_out" "$raw" >/dev/null
    [ -s "$raw_out" ] || { printf '%s\n' "missing raw smoke output: $raw_out (input: $raw)" >&2; exit 1; }
    rm -f "$raw_bad_res_log" "$raw_bad_res_out"
    if "$smoke_bin" --demuxer raw --input-res 16x16x --fps 25 --frames 1 --crf 30 -o "$raw_bad_res_out" "$raw" >"$raw_bad_res_log" 2>&1; then
        printf '%s\n' "accepted raw input resolution trailing junk: $raw_bad_res_out" >&2
        exit 1
    fi
    grep -q "invalid resolution" "$raw_bad_res_log" ||
    {
        printf '%s\n' "missing raw resolution parse error in $raw_bad_res_log" >&2
        exit 1
    }
    rm -f "$param_bad_fps_log" "$param_bad_fps_out"
    if "$smoke_bin" --demuxer raw --input-res 16x16 --fps 25/1x --frames 1 --crf 30 -o "$param_bad_fps_out" "$raw" >"$param_bad_fps_log" 2>&1; then
        printf '%s\n' "accepted parameter fps trailing junk: $param_bad_fps_out" >&2
        exit 1
    fi
    grep -q "invalid argument: fps" "$param_bad_fps_log" ||
    {
        printf '%s\n' "missing parameter fps parse error in $param_bad_fps_log" >&2
        exit 1
    }
    "$smoke_bin" --demuxer y4m --frames 1 --partitions p8x8,p4x4,b8x8,i8x8,i4x4 --crf 30 -o "$param_partitions_out" "$y4m" >/dev/null
    [ -s "$param_partitions_out" ] || { printf '%s\n' "missing partitions smoke output: $param_partitions_out (input: $y4m)" >&2; exit 1; }
    rm -f "$param_bad_partitions_log" "$param_bad_partitions_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --partitions p8x8junk --crf 30 -o "$param_bad_partitions_out" "$y4m" >"$param_bad_partitions_log" 2>&1; then
        printf '%s\n' "accepted partitions trailing junk: $param_bad_partitions_out" >&2
        exit 1
    fi
    grep -q "invalid argument: partitions" "$param_bad_partitions_log" ||
    {
        printf '%s\n' "missing parameter partitions parse error in $param_bad_partitions_log" >&2
        exit 1
    }
    rm -f "$param_bad_analyse_log" "$param_bad_analyse_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --analyse alljunk --crf 30 -o "$param_bad_analyse_out" "$y4m" >"$param_bad_analyse_log" 2>&1; then
        printf '%s\n' "accepted analyse trailing junk: $param_bad_analyse_out" >&2
        exit 1
    fi
    grep -q "invalid argument: analyse" "$param_bad_analyse_log" ||
    {
        printf '%s\n' "missing parameter analyse parse error in $param_bad_analyse_log" >&2
        exit 1
    }
    rm -f "$param_bad_deblock_log" "$param_bad_deblock_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --deblock 1:1x --crf 30 -o "$param_bad_deblock_out" "$y4m" >"$param_bad_deblock_log" 2>&1; then
        printf '%s\n' "accepted parameter deblock trailing junk: $param_bad_deblock_out" >&2
        exit 1
    fi
    grep -q "invalid argument: deblock" "$param_bad_deblock_log" ||
    {
        printf '%s\n' "missing parameter deblock parse error in $param_bad_deblock_log" >&2
        exit 1
    }
    rm -f "$param_bad_qpmin_log" "$param_bad_qpmin_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --qpmin 1:2,3 --crf 30 -o "$param_bad_qpmin_out" "$y4m" >"$param_bad_qpmin_log" 2>&1; then
        printf '%s\n' "accepted mixed-separator qpmin: $param_bad_qpmin_out" >&2
        exit 1
    fi
    grep -q "invalid argument: qpmin" "$param_bad_qpmin_log" ||
    {
        printf '%s\n' "missing parameter qpmin parse error in $param_bad_qpmin_log" >&2
        exit 1
    }
    "$smoke_bin" --demuxer y4m --frames 1 --ratetol inf --crf 30 -o "$param_ratetol_inf_out" "$y4m" >/dev/null
    [ -s "$param_ratetol_inf_out" ] || { printf '%s\n' "missing ratetol inf smoke output: $param_ratetol_inf_out (input: $y4m)" >&2; exit 1; }
    rm -f "$param_bad_ratetol_log" "$param_bad_ratetol_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --ratetol infjunk --crf 30 -o "$param_bad_ratetol_out" "$y4m" >"$param_bad_ratetol_log" 2>&1; then
        printf '%s\n' "accepted ratetol inf prefix junk: $param_bad_ratetol_out" >&2
        exit 1
    fi
    grep -q "invalid argument: ratetol" "$param_bad_ratetol_log" ||
    {
        printf '%s\n' "missing parameter ratetol parse error in $param_bad_ratetol_log" >&2
        exit 1
    }
    "$smoke_bin" --demuxer y4m --frames 1 --keyint infinite --crf 30 -o "$param_keyint_inf_out" "$y4m" >/dev/null
    [ -s "$param_keyint_inf_out" ] || { printf '%s\n' "missing keyint infinite smoke output: $param_keyint_inf_out (input: $y4m)" >&2; exit 1; }
    rm -f "$param_bad_keyint_log" "$param_bad_keyint_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --keyint infinitejunk --crf 30 -o "$param_bad_keyint_out" "$y4m" >"$param_bad_keyint_log" 2>&1; then
        printf '%s\n' "accepted keyint infinite prefix junk: $param_bad_keyint_out" >&2
        exit 1
    fi
    grep -q "invalid argument: keyint" "$param_bad_keyint_log" ||
    {
        printf '%s\n' "missing parameter keyint parse error in $param_bad_keyint_log" >&2
        exit 1
    }
    rm -f "$param_bad_mastering_log" "$param_bad_mastering_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --mastering-display 'G(1,2)B(3,4)R(5,6)WP(7,8)L(9,10)junk' --crf 30 -o "$param_bad_mastering_out" "$y4m" >"$param_bad_mastering_log" 2>&1; then
        printf '%s\n' "accepted mastering-display trailing junk: $param_bad_mastering_out" >&2
        exit 1
    fi
    grep -q "invalid argument: mastering-display" "$param_bad_mastering_log" ||
    {
        printf '%s\n' "missing parameter mastering-display parse error in $param_bad_mastering_log" >&2
        exit 1
    }
    rm -f "$param_bad_aq3_boundary_log" "$param_bad_aq3_boundary_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --aq3-boundary 192:64x24 --crf 30 -o "$param_bad_aq3_boundary_out" "$y4m" >"$param_bad_aq3_boundary_log" 2>&1; then
        printf '%s\n' "accepted malformed aq3-boundary: $param_bad_aq3_boundary_out" >&2
        exit 1
    fi
    grep -q "invalid argument: aq3-boundary" "$param_bad_aq3_boundary_log" ||
    {
        printf '%s\n' "missing parameter aq3-boundary parse error in $param_bad_aq3_boundary_log" >&2
        exit 1
    }
    rm -f "$param_bad_aq3_boundary_log" "$param_bad_aq3_boundary_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --aq3-boundary 192:64 --crf 30 -o "$param_bad_aq3_boundary_out" "$y4m" >"$param_bad_aq3_boundary_log" 2>&1; then
        printf '%s\n' "accepted short aq3-boundary list: $param_bad_aq3_boundary_out" >&2
        exit 1
    fi
    grep -q "invalid argument: aq3-boundary" "$param_bad_aq3_boundary_log" ||
    {
        printf '%s\n' "missing short aq3-boundary parse error in $param_bad_aq3_boundary_log" >&2
        exit 1
    }
    rm -f "$param_bad_psyrd_log" "$param_bad_psyrd_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --psy-rd 1.0:0.2junk --crf 30 -o "$param_bad_psyrd_out" "$y4m" >"$param_bad_psyrd_log" 2>&1; then
        printf '%s\n' "accepted psy-rd trailing junk: $param_bad_psyrd_out" >&2
        exit 1
    fi
    grep -q "invalid argument: psy-rd" "$param_bad_psyrd_log" ||
    {
        printf '%s\n' "missing parameter psy-rd parse error in $param_bad_psyrd_log" >&2
        exit 1
    }
    rm -f "$param_bad_aq3_strength_log" "$param_bad_aq3_strength_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --aq3-strength 0.1:0.2,0.3:0.4:0.5:0.6:0.7:0.8 --crf 30 -o "$param_bad_aq3_strength_out" "$y4m" >"$param_bad_aq3_strength_log" 2>&1; then
        printf '%s\n' "accepted mixed-separator aq3-strength: $param_bad_aq3_strength_out" >&2
        exit 1
    fi
    grep -q "invalid argument: aq3-strength" "$param_bad_aq3_strength_log" ||
    {
        printf '%s\n' "missing parameter aq3-strength parse error in $param_bad_aq3_strength_log" >&2
        exit 1
    }
    rm -f "$param_bad_aq3_ifactor_log" "$param_bad_aq3_ifactor_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --aq3-ifactor 1.0:inf --crf 30 -o "$param_bad_aq3_ifactor_out" "$y4m" >"$param_bad_aq3_ifactor_log" 2>&1; then
        printf '%s\n' "accepted aq3-ifactor inf: $param_bad_aq3_ifactor_out" >&2
        exit 1
    fi
    grep -q "invalid argument: aq3-ifactor" "$param_bad_aq3_ifactor_log" ||
    {
        printf '%s\n' "missing parameter aq3-ifactor parse error in $param_bad_aq3_ifactor_log" >&2
        exit 1
    }
    rm -f "$param_bad_cqm4_log" "$param_bad_cqm4_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --cqm4 "1x,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1" --crf 30 -o "$param_bad_cqm4_out" "$y4m" >"$param_bad_cqm4_log" 2>&1; then
        printf '%s\n' "accepted parameter cqm4 trailing junk: $param_bad_cqm4_out" >&2
        exit 1
    fi
    grep -q "invalid argument: cqm4" "$param_bad_cqm4_log" ||
    {
        printf '%s\n' "missing parameter cqm4 parse error in $param_bad_cqm4_log" >&2
        exit 1
    }
    rm -f "$filter_bad_pad_log" "$filter_bad_pad_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --vf pad:abc,0,0,0,0,0,0,0,0 --crf 30 -o "$filter_bad_pad_out" "$y4m" >"$filter_bad_pad_log" 2>&1; then
        printf '%s\n' "accepted malformed pad filter value: $filter_bad_pad_out" >&2
        exit 1
    fi
    grep -q "left pad value 'abc' is invalid" "$filter_bad_pad_log" ||
    {
        printf '%s\n' "missing pad filter parse error in $filter_bad_pad_log" >&2
        exit 1
    }
    "$smoke_bin" --demuxer y4m --frames 1 --cqm flat --crf 30 -o "$cqm_flat_out" "$y4m" >/dev/null
    [ -s "$cqm_flat_out" ] || { printf '%s\n' "missing CQM flat preset smoke output: $cqm_flat_out (input: $y4m)" >&2; exit 1; }
    "$smoke_bin" --demuxer y4m --frames 1 --cqm jvt --crf 30 -o "$cqm_jvt_out" "$y4m" >/dev/null
    [ -s "$cqm_jvt_out" ] || { printf '%s\n' "missing CQM JVT preset smoke output: $cqm_jvt_out (input: $y4m)" >&2; exit 1; }
    rm -f "$cqm_missing_flat" "$cqm_missing_flat_log" "$cqm_missing_flat_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --cqm "$cqm_missing_flat" --crf 30 -o "$cqm_missing_flat_out" "$y4m" >"$cqm_missing_flat_log" 2>&1; then
        printf '%s\n' "accepted missing CQM file path as preset substring: $cqm_missing_flat_out" >&2
        exit 1
    fi
    grep -q "can't open file" "$cqm_missing_flat_log" ||
    {
        printf '%s\n' "missing CQM file-open parse error in $cqm_missing_flat_log" >&2
        exit 1
    }
    {
        printf '%s\n' "XINTRA4X4_LUMA = -1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1"
        printf '%s\n' "INTRA4X4_LUMAX = -1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1"
    } > "$cqmfile_anchored"
    "$smoke_bin" --demuxer y4m --frames 1 --cqmfile "$cqmfile_anchored" --crf 30 -o "$cqmfile_anchored_out" "$y4m" >/dev/null
    [ -s "$cqmfile_anchored_out" ] || { printf '%s\n' "missing anchored CQM file smoke output: $cqmfile_anchored_out (input: $cqmfile_anchored)" >&2; exit 1; }
    printf '%s\n' "INTRA4X4_LUMA = -1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1" > "$cqmfile_bad"
    rm -f "$cqmfile_bad_log" "$cqmfile_bad_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --cqmfile "$cqmfile_bad" --crf 30 -o "$cqmfile_bad_out" "$y4m" >"$cqmfile_bad_log" 2>&1; then
        printf '%s\n' "accepted malformed CQM file coefficient: $cqmfile_bad" >&2
        exit 1
    fi
    grep -q "bad coefficient in list 'INTRA4X4_LUMA'" "$cqmfile_bad_log" ||
    {
        printf '%s\n' "missing CQM file coefficient parse error in $cqmfile_bad_log" >&2
        exit 1
    }
    printf '%s\n' '# timecode format v2junk' > "$tc_bad_header"
    rm -f "$tc_bad_header_log" "$tc_bad_header_out"
    if "$smoke_bin" --demuxer y4m --tcfile-in "$tc_bad_header" --frames 1 --crf 30 -o "$tc_bad_header_out" "$y4m" >"$tc_bad_header_log" 2>&1; then
        printf '%s\n' "accepted malformed timecode header: $tc_bad_header" >&2
        exit 1
    fi
    grep -q "unsupported timecode format" "$tc_bad_header_log" ||
    {
        printf '%s\n' "missing malformed timecode header parse error in $tc_bad_header_log" >&2
        exit 1
    }
    {
        printf '%s\n' '# timecode format v1'
        printf '%s\n' 'assume 25'
        printf '%s\n' '# TDecimate Mode 3: Last Frame = -1'
    } > "$tc_bad_tdecimate"
    rm -f "$tc_bad_tdecimate_log" "$tc_bad_tdecimate_out"
    if "$smoke_bin" --demuxer y4m --tcfile-in "$tc_bad_tdecimate" --frames 1 --crf 30 -o "$tc_bad_tdecimate_out" "$y4m" >"$tc_bad_tdecimate_log" 2>&1; then
        printf '%s\n' "accepted malformed TDecimate last-frame count: $tc_bad_tdecimate" >&2
        exit 1
    fi
    grep -q "invalid tcfile frame count" "$tc_bad_tdecimate_log" ||
    {
        printf '%s\n' "missing malformed TDecimate parse error in $tc_bad_tdecimate_log" >&2
        exit 1
    }
    rm -f "$stats_y4m" "$stats_valid" "$stats_pass1_out" \
          "$stats_bad_timebase" "$stats_bad_timebase_out" "$stats_bad_timebase_log" \
          "$stats_bad_main" "$stats_bad_main_out" "$stats_bad_main_log" \
          "$stats_bad_main_ref" "$stats_bad_main_ref_out" "$stats_bad_main_ref_log" \
          "$stats_bad_ref" "$stats_bad_ref_out" "$stats_bad_ref_log" \
          "$stats_weight" "$stats_weight_out" "$stats_weight_spaces" "$stats_weight_spaces_out" \
          "$stats_weight_chroma_spaces" "$stats_weight_chroma_spaces_out" \
          "$stats_bad_weight" "$stats_bad_weight_out" "$stats_bad_weight_log" \
          "$stats_bad_bframes" "$stats_bad_bframes_out" "$stats_bad_bframes_log" \
          "$stats_bad_bframes_ws" "$stats_bad_bframes_ws_out" "$stats_bad_bframes_ws_log" \
          "$stats_bad_lookahead_ws" "$stats_bad_lookahead_ws_out" "$stats_bad_lookahead_ws_log"
    write_smoke_y4m_frames "$stats_y4m" 2 25:1
    "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 1 --no-mbtree --bframes 0 --ref 1 --stats "$stats_valid" -o "$stats_pass1_out" "$stats_y4m" >/dev/null
    [ -s "$stats_valid" ] || { printf '%s\n' "missing two-pass stats smoke output: $stats_valid" >&2; exit 1; }
    sed '1s|timebase=[^ ]*|timebase=-1/1|' "$stats_valid" > "$stats_bad_timebase"
    if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_timebase" -o "$stats_bad_timebase_out" "$stats_y4m" >"$stats_bad_timebase_log" 2>&1; then
        printf '%s\n' "accepted malformed stats timebase: $stats_bad_timebase" >&2
        exit 1
    fi
    grep -q "timebase specified in stats file not valid" "$stats_bad_timebase_log" ||
    {
        printf '%s\n' "missing malformed stats timebase parse error in $stats_bad_timebase_log" >&2
        exit 1
    }
    sed '2s|q:[^ ]*|q:nan|' "$stats_valid" > "$stats_bad_main"
    if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_main" -o "$stats_bad_main_out" "$stats_y4m" >"$stats_bad_main_log" 2>&1; then
        printf '%s\n' "accepted malformed stats main fields: $stats_bad_main" >&2
        exit 1
    fi
    grep -q "statistics are damaged at line 0" "$stats_bad_main_log" ||
    {
        printf '%s\n' "missing malformed stats main-field parse error in $stats_bad_main_log" >&2
        exit 1
    }
    sed '3s| d:- ref:| d:- junk:1 ref:|' "$stats_valid" > "$stats_bad_main_ref"
    if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_main_ref" -o "$stats_bad_main_ref_out" "$stats_y4m" >"$stats_bad_main_ref_log" 2>&1; then
        printf '%s\n' "accepted malformed stats token before ref list: $stats_bad_main_ref" >&2
        exit 1
    fi
    grep -q "statistics are damaged at line 1" "$stats_bad_main_ref_log" ||
    {
        printf '%s\n' "missing malformed stats token-before-ref parse error in $stats_bad_main_ref_log" >&2
        exit 1
    }
    sed '3s|ref:[^ ]*|ref:0x|' "$stats_valid" > "$stats_bad_ref"
    if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_ref" -o "$stats_bad_ref_out" "$stats_y4m" >"$stats_bad_ref_log" 2>&1; then
        printf '%s\n' "accepted malformed stats ref list: $stats_bad_ref" >&2
        exit 1
    fi
    grep -q "statistics are damaged at line 1" "$stats_bad_ref_log" ||
    {
        printf '%s\n' "missing malformed stats ref-list parse error in $stats_bad_ref_log" >&2
        exit 1
    }
    sed '3s|ref:0 ;|ref:0 w:0,1,0 ;|' "$stats_valid" > "$stats_weight"
    "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_weight" -o "$stats_weight_out" "$stats_y4m" >/dev/null
    [ -s "$stats_weight_out" ] || { printf '%s\n' "missing stats weight smoke output: $stats_weight_out" >&2; exit 1; }
    sed '3s|ref:0 ;|ref:0 w:0, 1, 0 ;|' "$stats_valid" > "$stats_weight_spaces"
    "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_weight_spaces" -o "$stats_weight_spaces_out" "$stats_y4m" >/dev/null
    [ -s "$stats_weight_spaces_out" ] || { printf '%s\n' "missing stats weight-spaces smoke output: $stats_weight_spaces_out" >&2; exit 1; }
    sed '3s|ref:0 ;|ref:0 w:0, 1, 0, 0, 1, 0, 1, 0 ;|' "$stats_valid" > "$stats_weight_chroma_spaces"
    "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_weight_chroma_spaces" -o "$stats_weight_chroma_spaces_out" "$stats_y4m" >/dev/null
    [ -s "$stats_weight_chroma_spaces_out" ] || { printf '%s\n' "missing stats weight chroma-spaces smoke output: $stats_weight_chroma_spaces_out" >&2; exit 1; }
    sed '3s|ref:0 ;|ref:0 w:0,1,0x ;|' "$stats_valid" > "$stats_bad_weight"
    if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_weight" -o "$stats_bad_weight_out" "$stats_y4m" >"$stats_bad_weight_log" 2>&1; then
        printf '%s\n' "accepted malformed stats weight list: $stats_bad_weight" >&2
        exit 1
    fi
    grep -q "statistics are damaged at line 1" "$stats_bad_weight_log" ||
    {
        printf '%s\n' "missing malformed stats weight-list parse error in $stats_bad_weight_log" >&2
        exit 1
    }
    sed '1s|bframes=[^ ]*|bframes=3x|' "$stats_valid" > "$stats_bad_bframes"
    if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_bframes" -o "$stats_bad_bframes_out" "$stats_y4m" >"$stats_bad_bframes_log" 2>&1; then
        printf '%s\n' "accepted malformed stats bframes token: $stats_bad_bframes" >&2
        exit 1
    fi
    grep -q "bframes specified in stats file not valid" "$stats_bad_bframes_log" ||
    {
        printf '%s\n' "missing malformed stats bframes parse error in $stats_bad_bframes_log" >&2
        exit 1
    }
    sed '1s|bframes=[^ ]*|bframes= 3|' "$stats_valid" > "$stats_bad_bframes_ws"
    if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_bframes_ws" -o "$stats_bad_bframes_ws_out" "$stats_y4m" >"$stats_bad_bframes_ws_log" 2>&1; then
        printf '%s\n' "accepted whitespace-prefixed stats bframes token: $stats_bad_bframes_ws" >&2
        exit 1
    fi
    grep -q "bframes specified in stats file not valid" "$stats_bad_bframes_ws_log" ||
    {
        printf '%s\n' "missing whitespace-prefixed stats bframes parse error in $stats_bad_bframes_ws_log" >&2
        exit 1
    }
    sed '1s|$| rc_lookahead= 1|' "$stats_valid" > "$stats_bad_lookahead_ws"
    if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --vbv-bufsize 100 --vbv-maxrate 100 --stats "$stats_bad_lookahead_ws" -o "$stats_bad_lookahead_ws_out" "$stats_y4m" >"$stats_bad_lookahead_ws_log" 2>&1; then
        printf '%s\n' "accepted whitespace-prefixed stats rc_lookahead token: $stats_bad_lookahead_ws" >&2
        exit 1
    fi
    grep -q "rc_lookahead specified in stats file not valid" "$stats_bad_lookahead_ws_log" ||
    {
        printf '%s\n' "missing whitespace-prefixed stats rc_lookahead parse error in $stats_bad_lookahead_ws_log" >&2
        exit 1
    }
    write_smoke_y4m_with_header "$y4m_unknown" 1 "YUV4MPEG2 W16 H16 F0:0 Ip A0:0 C420"
    "$smoke_bin" --demuxer y4m --fps 25 --frames 1 --crf 30 -o "$y4m_unknown_out" "$y4m_unknown" >/dev/null
    [ -s "$y4m_unknown_out" ] || { printf '%s\n' "missing Y4M unknown-ratio smoke output: $y4m_unknown_out (input: $y4m_unknown)" >&2; exit 1; }
    write_smoke_y4m_with_header "$y4m_420jpeg" 1 "YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420jpeg"
    "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$y4m_420jpeg_out" "$y4m_420jpeg" >/dev/null
    [ -s "$y4m_420jpeg_out" ] || { printf '%s\n' "missing Y4M 420jpeg smoke output: $y4m_420jpeg_out (input: $y4m_420jpeg)" >&2; exit 1; }
    write_smoke_y4m_with_header "$y4m_bad_fps" 1 "YUV4MPEG2 W16 H16 F25:1x Ip A1:1 C420"
    rm -f "$y4m_bad_fps_log" "$y4m_bad_fps_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$y4m_bad_fps_out" "$y4m_bad_fps" >"$y4m_bad_fps_log" 2>&1; then
        printf '%s\n' "accepted Y4M frame-rate trailing junk: $y4m_bad_fps (output: $y4m_bad_fps_out)" >&2
        exit 1
    fi
    grep -q "invalid frame rate" "$y4m_bad_fps_log" ||
    {
        printf '%s\n' "missing Y4M frame-rate parse error in $y4m_bad_fps_log" >&2
        exit 1
    }
    write_smoke_y4m_with_header "$y4m_bad_csp_suffix" 1 "YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420junk"
    rm -f "$y4m_bad_csp_suffix_log" "$y4m_bad_csp_suffix_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$y4m_bad_csp_suffix_out" "$y4m_bad_csp_suffix" >"$y4m_bad_csp_suffix_log" 2>&1; then
        printf '%s\n' "accepted Y4M colorspace suffix junk: $y4m_bad_csp_suffix (output: $y4m_bad_csp_suffix_out)" >&2
        exit 1
    fi
    grep -q "colorspace unhandled" "$y4m_bad_csp_suffix_log" ||
    {
        printf '%s\n' "missing Y4M colorspace suffix parse error in $y4m_bad_csp_suffix_log" >&2
        exit 1
    }
    write_smoke_y4m_with_header "$y4m_bad_width" 1 "YUV4MPEG2 W16x H16 F25:1 Ip A1:1 C420"
    rm -f "$y4m_bad_width_log" "$y4m_bad_width_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$y4m_bad_width_out" "$y4m_bad_width" >"$y4m_bad_width_log" 2>&1; then
        printf '%s\n' "accepted Y4M width trailing junk: $y4m_bad_width (output: $y4m_bad_width_out)" >&2
        exit 1
    fi
    grep -q "invalid width" "$y4m_bad_width_log" ||
    {
        printf '%s\n' "missing Y4M width parse error in $y4m_bad_width_log" >&2
        exit 1
    }
    write_smoke_y4m_with_header "$y4m_bad_height" 1 "YUV4MPEG2 W16 H16x F25:1 Ip A1:1 C420"
    rm -f "$y4m_bad_height_log" "$y4m_bad_height_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$y4m_bad_height_out" "$y4m_bad_height" >"$y4m_bad_height_log" 2>&1; then
        printf '%s\n' "accepted Y4M height trailing junk: $y4m_bad_height (output: $y4m_bad_height_out)" >&2
        exit 1
    fi
    grep -q "invalid height" "$y4m_bad_height_log" ||
    {
        printf '%s\n' "missing Y4M height parse error in $y4m_bad_height_log" >&2
        exit 1
    }
    write_smoke_y4m_with_header "$y4m_bad_length" 1 "YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420 XLENGTH=1x"
    rm -f "$y4m_bad_length_log" "$y4m_bad_length_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$y4m_bad_length_out" "$y4m_bad_length" >"$y4m_bad_length_log" 2>&1; then
        printf '%s\n' "accepted Y4M XLENGTH trailing junk: $y4m_bad_length (output: $y4m_bad_length_out)" >&2
        exit 1
    fi
    grep -q "invalid frame count" "$y4m_bad_length_log" ||
    {
        printf '%s\n' "missing Y4M XLENGTH parse error in $y4m_bad_length_log" >&2
        exit 1
    }
    write_smoke_y4m_with_header "$y4m_bad_color_range" 1 "YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420 XCOLORRANGE=FULLjunk"
    rm -f "$y4m_bad_color_range_log" "$y4m_bad_color_range_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$y4m_bad_color_range_out" "$y4m_bad_color_range" >"$y4m_bad_color_range_log" 2>&1; then
        printf '%s\n' "accepted Y4M color range trailing junk: $y4m_bad_color_range (output: $y4m_bad_color_range_out)" >&2
        exit 1
    fi
    grep -q "invalid color range" "$y4m_bad_color_range_log" ||
    {
        printf '%s\n' "missing Y4M color range parse error in $y4m_bad_color_range_log" >&2
        exit 1
    }
    rm -f "$zone_nan_log" "$zone_nan_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --zones "0,0,b=nan" --crf 30 -o "$zone_nan_out" "$y4m" >"$zone_nan_log" 2>&1; then
        printf '%s\n' "accepted zone NaN bitrate factor: $zone_nan_out" >&2
        exit 1
    fi
    grep -q "invalid zone bitrate factor" "$zone_nan_log" ||
    {
        printf '%s\n' "missing zone NaN parse error in $zone_nan_log" >&2
        exit 1
    }
    printf '%s\n' '0 I none' > "$qp_none"
    "$smoke_bin" --quiet --demuxer y4m --frames 1 --qpfile "$qp_none" -o "$qp_none_out" "$y4m" >/dev/null
    [ -s "$qp_none_out" ] || { printf '%s\n' "missing qpfile none smoke output: $qp_none_out (input: $y4m)" >&2; exit 1; }
    {
        printf '%s' '0 I '
        printf '%120s\n' '' | tr ' ' '1'
    } > "$qp_trailing"
    rm -f "$qp_trailing_log" "$qp_trailing_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --qpfile "$qp_trailing" -o "$qp_trailing_out" "$y4m" >"$qp_trailing_log" 2>&1; then
        :
    fi
    grep -q "can't parse qpfile for frame 0" "$qp_trailing_log" ||
    {
        printf '%s\n' "missing qpfile long-line parse error in $qp_trailing_log" >&2
        exit 1
    }
    printf '%s\n' '0 I none trailing' > "$qp_trailing"
    rm -f "$qp_trailing_log" "$qp_trailing_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --qpfile "$qp_trailing" -o "$qp_trailing_out" "$y4m" >"$qp_trailing_log" 2>&1; then
        :
    fi
    grep -q "can't parse qpfile for frame 0" "$qp_trailing_log" ||
    {
        printf '%s\n' "missing qpfile trailing-junk parse error in $qp_trailing_log" >&2
        exit 1
    }
    printf '%s\n' "smoke-cli output: $out ($(wc -c < "$out") bytes), qpfile none: $qp_none_out ($(wc -c < "$qp_none_out") bytes)"
}

run_smoke_output_mp4()
{
    require_ffprobe smoke-output:mp4
    require_pkg_config_package smoke-output:mp4 liblsmash
    configure_smoke_cli smoke-output-mp4 "--enable-lsmash --disable-lavf --disable-ffms --disable-audio"
    y4m=$smoke_dir/smoke.y4m
    out=$smoke_dir/smoke.mp4
    write_smoke_y4m "$y4m"
    "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$out" "$y4m" >/dev/null
    pix_fmt=$(ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt -of csv=p=0 "$out")
    frames=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$out")
    [ "$pix_fmt" = yuv420p ] || { printf '%s\n' "unexpected mp4 pix_fmt: $pix_fmt" >&2; exit 1; }
    [ "$frames" = 1 ] || { printf '%s\n' "unexpected mp4 frame count: $frames" >&2; exit 1; }
    printf '%s\n' "smoke-output:mp4 output: $out pix_fmt=$pix_fmt frames=$frames"
}

run_smoke_output_gop()
{
    configure_smoke_cli smoke-output-gop "--disable-lavf --disable-ffms --disable-lsmash --disable-audio"
    y4m=$smoke_dir/smoke.y4m
    out=$smoke_dir/smoke.gop
    base=${out%.gop}
    rm -f "$out" "$base.options" "$base.headers" "$base-000000.264-gop-data"
    write_smoke_y4m "$y4m"
    "$smoke_bin" --demuxer y4m --frames 1 --crf 30 --muxer gop -o "$out" "$y4m" >/dev/null
    [ -s "$out" ] || { printf '%s\n' "missing GOP index: $out (build dir: $smoke_dir)" >&2; exit 1; }
    [ -s "$base.options" ] || { printf '%s\n' "missing GOP options: $base.options (build dir: $smoke_dir)" >&2; exit 1; }
    [ -s "$base.headers" ] || { printf '%s\n' "missing GOP headers: $base.headers (build dir: $smoke_dir)" >&2; exit 1; }
    [ -s "$base-000000.264-gop-data" ] || { printf '%s\n' "missing GOP data: $base-000000.264-gop-data (build dir: $smoke_dir)" >&2; exit 1; }
    printf '%s\n' "smoke-output:gop outputs: $out $base.options $base.headers $base-000000.264-gop-data"
}

run_smoke_output_mkv()
{
    require_ffprobe smoke-output:mkv
    configure_smoke_cli smoke-output-mkv "--disable-lavf --disable-ffms --disable-lsmash --disable-audio"
    y4m=$smoke_dir/smoke.y4m
    out=$smoke_dir/smoke.mkv
    write_smoke_y4m "$y4m"
    "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$out" "$y4m" >/dev/null
    [ -s "$out" ] || { printf '%s\n' "missing Matroska smoke output: $out (input: $y4m)" >&2; exit 1; }
    frames=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$out")
    duration=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$out")
    [ "$frames" = 1 ] || { printf '%s\n' "unexpected mkv frame count: $frames" >&2; exit 1; }
    assert_duration_between smoke-output:mkv "$duration" 0.030000 0.060000
    printf '%s\n' "smoke-output:mkv output: $out frames=$frames duration=$duration"
}

run_smoke_output_flv()
{
    require_ffprobe smoke-output:flv
    configure_smoke_cli smoke-output-flv "--disable-lavf --disable-ffms --disable-lsmash --disable-audio"
    y4m=$smoke_dir/smoke-vfr.y4m
    tcfile=$smoke_dir/smoke-vfr.tc
    out=$smoke_dir/smoke.flv
    write_smoke_y4m_frames "$y4m" 3 25:1
    {
        printf '%s\n' '# timecode format v2'
        printf '%s\n' '0'
        printf '%s\n' '40'
        printf '%s\n' '120'
    } > "$tcfile"
    "$smoke_bin" --demuxer y4m --tcfile-in "$tcfile" --timebase 1/1000 --seek 1 --frames 2 \
        --bframes 0 --sync-lookahead 0 --rc-lookahead 0 --crf 30 -o "$out" "$y4m" >/dev/null
    [ -s "$out" ] || { printf '%s\n' "missing FLV smoke output: $out (input: $y4m tcfile: $tcfile)" >&2; exit 1; }
    frames=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$out")
    duration=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$out")
    [ "$frames" = 2 ] || { printf '%s\n' "unexpected flv frame count: $frames" >&2; exit 1; }
    assert_duration_between smoke-output:flv "$duration" 0.001000 1.000000
    printf '%s\n' "smoke-output:flv output: $out frames=$frames duration=$duration"
}

run_smoke_output()
{
    run_smoke_output_mp4
    run_smoke_output_gop
    run_smoke_output_mkv
    run_smoke_output_flv
}

run_shared_consumer_depth()
{
    label=$1
    bit_depth=$2
    build_dir=$build_root/shared-$label
    prefix=$build_root/install-consumer/shared-$label
    sysroot=$prefix/root
    source=$prefix/consumer.c
    binary=$prefix/consumer.exe
    strict_source=$prefix/strict-consumer.c
    strict_binary=$prefix/strict-consumer.exe

    mkdir -p "$build_dir" "$prefix"
    (
        cd "$build_dir"
        PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-$msys2_pkg_config_path} \
        CC=${CC:-cc} RC= "$root/configure" --enable-shared --disable-cli --disable-asm \
            --disable-audio --disable-avs --disable-lavf --disable-ffms --disable-lsmash --chroma-format=420 --bit-depth="$bit_depth"
        make -j"$make_jobs" lib-shared
        make install-lib-shared DESTDIR="$sysroot"
    )

    cflags=$(env -u PKG_CONFIG_PATH \
        PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=1 \
        PKG_CONFIG_LIBDIR="$sysroot/usr/local/lib/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="$sysroot" \
        pkg-config --cflags x264)
    staged_cflags="-I$sysroot/usr/local/include"
    pkg_config_libs=$(env -u PKG_CONFIG_PATH \
        PKG_CONFIG_ALLOW_SYSTEM_LIBS=1 \
        PKG_CONFIG_LIBDIR="$sysroot/usr/local/lib/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="$sysroot" \
        pkg-config --libs x264)
    libs="$sysroot/usr/local/lib/libx264.dll.a"
    dll_name=$(awk -F= '/^SONAME=/ { print $2 }' "$build_dir/config.mak")
    dll="$sysroot/usr/local/bin/$dll_name"
    [ -s "$libs" ] || { printf '%s\n' "missing shared import library: $libs" >&2; exit 1; }
    [ -s "$dll" ] || { printf '%s\n' "missing shared runtime library: $dll" >&2; exit 1; }
    assert_pkg_config_install_paths "$label shared" "$sysroot" "$cflags" "$pkg_config_libs"
    assert_token_present "$label shared pkg-config cflags" "$cflags" "-DX264_API_IMPORTS"
    assert_first_include_flag "$label shared explicit include order" "$staged_cflags" $staged_cflags $cflags

    {
        printf '%s\n' '#include <stdint.h>'
        printf '%s\n' '#include <x264.h>'
        printf '%s\n' "#if !defined(X264_BIT_DEPTH) || X264_BIT_DEPTH != $bit_depth"
        printf '%s\n' '#error unexpected installed shared bit depth'
        printf '%s\n' '#endif'
        printf '%s\n' 'int main(void)'
        printf '%s\n' '{'
        printf '%s\n' '    (void)x264_chroma_format;'
        printf '%s\n' '    return 0;'
        printf '%s\n' '}'
    } > "$source"

    cc -std=gnu17 -Wall -Wextra -Werror "$source" $staged_cflags $cflags "$libs" -o "$binary"
    PATH="$sysroot/usr/local/bin:$PATH" "$binary"
    cc -std=gnu17 -Wall -Wextra -Werror "$source" $staged_cflags $cflags $pkg_config_libs -o "$binary.pkg-config"
    PATH="$sysroot/usr/local/bin:$PATH" "$binary.pkg-config"

    {
        printf '%s\n' '#include <stdint.h>'
        printf '%s\n' '#include <x264.h>'
        printf '%s\n' '#if !defined(X264_API_IMPORTS)'
        printf '%s\n' '#error shared pkg-config cflags must request API imports'
        printf '%s\n' '#endif'
        printf '%s\n' '#if defined(_GNU_SOURCE) || defined(_POSIX_C_SOURCE)'
        printf '%s\n' '#error installed shared cflags must not force feature-test macros'
        printf '%s\n' '#endif'
        printf '%s\n' '#if !defined(X264_BIT_DEPTH) || !defined(X264_CHROMA_FORMAT)'
        printf '%s\n' '#error missing installed shared config macros'
        printf '%s\n' '#endif'
        printf '%s\n' 'int main(void)'
        printf '%s\n' '{'
        printf '%s\n' '    (void)x264_chroma_format;'
        printf '%s\n' '    return 0;'
        printf '%s\n' '}'
    } > "$strict_source"
    cc -std=c17 -pedantic -Wall -Wextra -Werror "$strict_source" $staged_cflags $cflags "$libs" -o "$strict_binary"
    PATH="$sysroot/usr/local/bin:$PATH" "$strict_binary"
    printf '%s\n' "shared consumer ok: label=$label import=$libs dll=$dll"
}
run_shared_consumers()
{
    run_shared_consumer_depth 8b 8
    run_shared_consumer_depth 10b 10
}

run_checkasm_smoke_depth()
{
    label=$1
    bit_depth=$2
    build_dir=$build_root/checkasm-$label

    mkdir -p "$build_dir"
    (
        cd "$build_dir"
        PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-$msys2_pkg_config_path} \
        CC=${CC:-cc} RC= "$root/configure" --enable-static --disable-cli \
            --disable-audio --disable-avs --disable-lavf --disable-ffms --disable-lsmash --chroma-format=420 --bit-depth="$bit_depth"
        make -j"$make_jobs" checkasm
        if [ -x ./checkasm${bit_depth}.exe ]; then
            ./checkasm${bit_depth}.exe 0
        else
            printf '%s\n' "missing checkasm${bit_depth}.exe in $build_dir" >&2
            exit 1
        fi
    )
    printf '%s\n' "checkasm smoke ok: label=$label bit_depth=$bit_depth"
}

run_checkasm_smoke()
{
    run_checkasm_smoke_depth 8b 8
    run_checkasm_smoke_depth 10b 10
}

case "$cmd" in
    whitespace) run_whitespace ;;
    builds) run_builds ;;
    consumers) run_consumers ;;
    warnings) run_warnings ;;
    warnings-extra) run_warnings_extra ;;
    warnings-extra:double) run_warnings_extra_double ;;
    warnings-extra:conversion) run_warnings_extra_conversion ;;
    warnings-extra:external) run_warnings_extra_external ;;
    warnings-extra:compare) run_warnings_extra_compare ;;
    warnings-extra:baseline-save) run_warnings_extra_baseline_save ;;
    warnings-extra:delta) run_warnings_extra_delta ;;
    feature-probe) run_feature_probe ;;
    avi-legacy-probe) run_avi_legacy_probe ;;
    smoke-cli) run_smoke_cli ;;
    smoke-output|smoke-output:all) run_smoke_output ;;
    smoke-output:mp4) run_smoke_output_mp4 ;;
    smoke-output:gop) run_smoke_output_gop ;;
    smoke-output:mkv) run_smoke_output_mkv ;;
    smoke-output:flv) run_smoke_output_flv ;;
    shared|shared-consumer) run_shared_consumer_depth 10b 10 ;;
    shared-8b) run_shared_consumer_depth 8b 8 ;;
    shared-consumers) run_shared_consumers ;;
    checkasm-smoke) run_checkasm_smoke ;;
    checkasm-smoke-8b) run_checkasm_smoke_depth 8b 8 ;;
    checkasm-smoke-10b) run_checkasm_smoke_depth 10b 10 ;;
    all)
        run_whitespace
        run_builds
        run_consumers
        run_warnings
        run_warnings_extra
        run_feature_probe
        ;;
    *)
        printf '%s\n' "usage: $0 [whitespace|builds|consumers|warnings|warnings-extra|warnings-extra:double|warnings-extra:conversion|warnings-extra:external|warnings-extra:compare|warnings-extra:baseline-save|warnings-extra:delta|feature-probe|avi-legacy-probe|smoke-cli|smoke-output|smoke-output:mp4|smoke-output:gop|smoke-output:mkv|smoke-output:flv|shared|shared-consumer|shared-8b|shared-consumers|checkasm-smoke|checkasm-smoke-8b|checkasm-smoke-10b|all]" >&2
        exit 2
        ;;
esac
