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
#include <stddef.h>
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

static int expect_guarded_value( const char *name, const char *text, int expected )
{
    int ret = parse_guarded_value( name, text );
    if( ret != expected )
    {
        fprintf( stderr, "guarded parse mismatch for %s=%s: got %d expected %d\n",
                 name, text, ret, expected );
        return -1;
    }

    return 0;
}

static int expect_unchanged_int_value( const char *name, const char *bad_text,
                                       const char *good_text, size_t field_offset )
{
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( bad_text, &allocation );
    if( !value )
        return -1;

    x264_param_default( &param );
    if( x264_param_parse( &param, name, good_text ) )
    {
        fprintf( stderr, "baseline parse failed for %s=%s\n", name, good_text );
        free_guarded_string( &allocation );
        return -1;
    }

    int *field = (int *)((char *)&param + field_offset);
    int before = *field;
    int ret = x264_param_parse( &param, name, value );
    int after = *field;
    free_guarded_string( &allocation );
    if( ret != X264_PARAM_BAD_VALUE || after != before )
    {
        fprintf( stderr, "guarded failed parse changed %s: ret %d value %d -> %d\n",
                 name, ret, before, after );
        return -1;
    }

    return 0;
}

static int expect_unchanged_uint32_value( const char *name, const char *bad_text,
                                          const char *good_text, size_t field_offset )
{
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( bad_text, &allocation );
    if( !value )
        return -1;

    x264_param_default( &param );
    if( x264_param_parse( &param, name, good_text ) )
    {
        fprintf( stderr, "baseline parse failed for %s=%s\n", name, good_text );
        free_guarded_string( &allocation );
        return -1;
    }

    uint32_t *field = (uint32_t *)((char *)&param + field_offset);
    uint32_t before = *field;
    int ret = x264_param_parse( &param, name, value );
    uint32_t after = *field;
    free_guarded_string( &allocation );
    if( ret != X264_PARAM_BAD_VALUE || after != before )
    {
        fprintf( stderr, "guarded failed parse changed %s: ret %d value %u -> %u\n",
                 name, ret, before, after );
        return -1;
    }

    return 0;
}

static int expect_apply_profile_failure_keeps_values( void )
{
    x264_param_t param;

    x264_param_default( &param );
    int profile_before = param.i_profile;
    int level_before = param.i_level_idc;
    int level_force_before = param.b_level_force;
    int bluray_before = param.b_bluray_compat;
    int vbv_maxrate_before = param.rc.i_vbv_max_bitrate;
    int vbv_bufsize_before = param.rc.i_vbv_buffer_size;
    int ret = x264_param_apply_profile( &param, NULL, "bluray,baddevice" );
    if( ret >= 0 ||
        param.i_profile != profile_before ||
        param.i_level_idc != level_before ||
        param.b_level_force != level_force_before ||
        param.b_bluray_compat != bluray_before ||
        param.rc.i_vbv_max_bitrate != vbv_maxrate_before ||
        param.rc.i_vbv_buffer_size != vbv_bufsize_before )
    {
        fprintf( stderr, "failed profile device parse changed values: ret %d profile %d -> %d level %d -> %d level-force %d -> %d bluray %d -> %d vbv %d/%d -> %d/%d\n",
                 ret, profile_before, param.i_profile,
                 level_before, param.i_level_idc,
                 level_force_before, param.b_level_force,
                 bluray_before, param.b_bluray_compat,
                 vbv_maxrate_before, vbv_bufsize_before,
                 param.rc.i_vbv_max_bitrate, param.rc.i_vbv_buffer_size );
        return -1;
    }

    x264_param_default( &param );
    param.b_interlaced = 1;
    int transform_8x8_before = param.analyse.b_transform_8x8;
    int cabac_before = param.b_cabac;
    int cqm_before = param.i_cqm_preset;
    int bframes_before = param.i_bframe;
    int weightp_before = param.analyse.i_weighted_pred;
    ret = x264_param_apply_profile( &param, "baseline", NULL );
    if( ret >= 0 ||
        param.analyse.b_transform_8x8 != transform_8x8_before ||
        param.b_cabac != cabac_before ||
        param.i_cqm_preset != cqm_before ||
        param.i_bframe != bframes_before ||
        param.analyse.i_weighted_pred != weightp_before )
    {
        fprintf( stderr, "failed baseline profile parse changed values: ret %d 8x8 %d -> %d cabac %d -> %d cqm %d -> %d bframes %d -> %d weightp %d -> %d\n",
                 ret, transform_8x8_before, param.analyse.b_transform_8x8,
                 cabac_before, param.b_cabac,
                 cqm_before, param.i_cqm_preset,
                 bframes_before, param.i_bframe,
                 weightp_before, param.analyse.i_weighted_pred );
        return -1;
    }

    return 0;
}

static int expect_failed_mastering_display_parse_keeps_values( void )
{
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( "G(11,+22)B(33,44)R(55,66)WP(77,88)L(99,100)", &allocation );
    if( !value )
        return -1;

    x264_param_default( &param );
    if( x264_param_parse( &param, "mastering-display", "G(1,2)B(3,4)R(5,6)WP(7,8)L(9,10)" ) )
    {
        fprintf( stderr, "baseline parse failed for mastering-display\n" );
        free_guarded_string( &allocation );
        return -1;
    }

    int enabled_before = param.mastering_display.b_mastering_display;
    int green_x_before = param.mastering_display.i_green_x;
    int green_y_before = param.mastering_display.i_green_y;
    int blue_x_before = param.mastering_display.i_blue_x;
    int blue_y_before = param.mastering_display.i_blue_y;
    int red_x_before = param.mastering_display.i_red_x;
    int red_y_before = param.mastering_display.i_red_y;
    int white_x_before = param.mastering_display.i_white_x;
    int white_y_before = param.mastering_display.i_white_y;
    int64_t display_max_before = param.mastering_display.i_display_max;
    int64_t display_min_before = param.mastering_display.i_display_min;
    int ret = x264_param_parse( &param, "mastering-display", value );
    free_guarded_string( &allocation );
    if( ret != X264_PARAM_BAD_VALUE ||
        param.mastering_display.b_mastering_display != enabled_before ||
        param.mastering_display.i_green_x != green_x_before ||
        param.mastering_display.i_green_y != green_y_before ||
        param.mastering_display.i_blue_x != blue_x_before ||
        param.mastering_display.i_blue_y != blue_y_before ||
        param.mastering_display.i_red_x != red_x_before ||
        param.mastering_display.i_red_y != red_y_before ||
        param.mastering_display.i_white_x != white_x_before ||
        param.mastering_display.i_white_y != white_y_before ||
        param.mastering_display.i_display_max != display_max_before ||
        param.mastering_display.i_display_min != display_min_before )
    {
        fprintf( stderr, "guarded failed parse changed mastering-display: ret %d enabled %d -> %d green %d,%d -> %d,%d blue %d,%d -> %d,%d red %d,%d -> %d,%d white %d,%d -> %d,%d L %lld,%lld -> %lld,%lld\n",
                 ret,
                 enabled_before, param.mastering_display.b_mastering_display,
                 green_x_before, green_y_before,
                 param.mastering_display.i_green_x, param.mastering_display.i_green_y,
                 blue_x_before, blue_y_before,
                 param.mastering_display.i_blue_x, param.mastering_display.i_blue_y,
                 red_x_before, red_y_before,
                 param.mastering_display.i_red_x, param.mastering_display.i_red_y,
                 white_x_before, white_y_before,
                 param.mastering_display.i_white_x, param.mastering_display.i_white_y,
                 (long long)display_max_before, (long long)display_min_before,
                 (long long)param.mastering_display.i_display_max,
                 (long long)param.mastering_display.i_display_min );
        return -1;
    }

    return 0;
}

static int expect_unchanged_float_value( const char *name, const char *bad_text,
                                         const char *good_text, size_t field_offset )
{
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( bad_text, &allocation );
    if( !value )
        return -1;

    x264_param_default( &param );
    if( x264_param_parse( &param, name, good_text ) )
    {
        fprintf( stderr, "baseline parse failed for %s=%s\n", name, good_text );
        free_guarded_string( &allocation );
        return -1;
    }

    float *field = (float *)((char *)&param + field_offset);
    float before = *field;
    int ret = x264_param_parse( &param, name, value );
    float after = *field;
    free_guarded_string( &allocation );
    if( ret != X264_PARAM_BAD_VALUE || after != before )
    {
        fprintf( stderr, "guarded failed parse changed %s: ret %d value %.9g -> %.9g\n",
                 name, ret, before, after );
        return -1;
    }

    return 0;
}

static int expect_failed_parse_keeps_int_default( const char *name, const char *bad_text, size_t field_offset )
{
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( bad_text, &allocation );
    if( !value )
        return -1;

    x264_param_default( &param );
    int *field = (int *)((char *)&param + field_offset);
    int before = *field;
    int ret = x264_param_parse( &param, name, value );
    int after = *field;
    free_guarded_string( &allocation );
    if( ret != X264_PARAM_BAD_VALUE || after != before )
    {
        fprintf( stderr, "guarded failed parse changed default %s: ret %d value %d -> %d\n",
                 name, ret, before, after );
        return -1;
    }

    return 0;
}

static int expect_failed_parse_keeps_two_int_values( const char *name, const char *bad_text,
                                                     const char *good_text,
                                                     size_t first_field_offset, size_t second_field_offset )
{
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( bad_text, &allocation );
    if( !value )
        return -1;

    x264_param_default( &param );
    if( x264_param_parse( &param, name, good_text ) )
    {
        fprintf( stderr, "baseline parse failed for %s=%s\n", name, good_text );
        free_guarded_string( &allocation );
        return -1;
    }

    int *first_field = (int *)((char *)&param + first_field_offset);
    int *second_field = (int *)((char *)&param + second_field_offset);
    int first_before = *first_field;
    int second_before = *second_field;
    int ret = x264_param_parse( &param, name, value );
    int first_after = *first_field;
    int second_after = *second_field;
    free_guarded_string( &allocation );
    if( ret != X264_PARAM_BAD_VALUE || first_after != first_before || second_after != second_before )
    {
        fprintf( stderr, "guarded failed parse changed %s: ret %d first %d -> %d second %d -> %d\n",
                 name, ret, first_before, first_after, second_before, second_after );
        return -1;
    }

    return 0;
}

static int expect_failed_parse_keeps_three_int_values( const char *name, const char *bad_text,
                                                       const char *good_text,
                                                       size_t first_field_offset, size_t second_field_offset,
                                                       size_t third_field_offset )
{
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( bad_text, &allocation );
    if( !value )
        return -1;

    x264_param_default( &param );
    if( x264_param_parse( &param, name, good_text ) )
    {
        fprintf( stderr, "baseline parse failed for %s=%s\n", name, good_text );
        free_guarded_string( &allocation );
        return -1;
    }

    int *first_field = (int *)((char *)&param + first_field_offset);
    int *second_field = (int *)((char *)&param + second_field_offset);
    int *third_field = (int *)((char *)&param + third_field_offset);
    int first_before = *first_field;
    int second_before = *second_field;
    int third_before = *third_field;
    int ret = x264_param_parse( &param, name, value );
    int first_after = *first_field;
    int second_after = *second_field;
    int third_after = *third_field;
    free_guarded_string( &allocation );
    if( ret != X264_PARAM_BAD_VALUE ||
        first_after != first_before || second_after != second_before || third_after != third_before )
    {
        fprintf( stderr, "guarded failed parse changed %s: ret %d first %d -> %d second %d -> %d third %d -> %d\n",
                 name, ret, first_before, first_after, second_before, second_after,
                 third_before, third_after );
        return -1;
    }

    return 0;
}

static int expect_failed_parse_keeps_four_int_values( const char *name, const char *bad_text,
                                                      const char *good_text,
                                                      size_t first_field_offset, size_t second_field_offset,
                                                      size_t third_field_offset, size_t fourth_field_offset )
{
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( bad_text, &allocation );
    if( !value )
        return -1;

    x264_param_default( &param );
    if( x264_param_parse( &param, name, good_text ) )
    {
        fprintf( stderr, "baseline parse failed for %s=%s\n", name, good_text );
        free_guarded_string( &allocation );
        return -1;
    }

    int *first_field = (int *)((char *)&param + first_field_offset);
    int *second_field = (int *)((char *)&param + second_field_offset);
    int *third_field = (int *)((char *)&param + third_field_offset);
    int *fourth_field = (int *)((char *)&param + fourth_field_offset);
    int first_before = *first_field;
    int second_before = *second_field;
    int third_before = *third_field;
    int fourth_before = *fourth_field;
    int ret = x264_param_parse( &param, name, value );
    int first_after = *first_field;
    int second_after = *second_field;
    int third_after = *third_field;
    int fourth_after = *fourth_field;
    free_guarded_string( &allocation );
    if( ret != X264_PARAM_BAD_VALUE ||
        first_after != first_before || second_after != second_before ||
        third_after != third_before || fourth_after != fourth_before )
    {
        fprintf( stderr, "guarded failed parse changed %s: ret %d first %d -> %d second %d -> %d third %d -> %d fourth %d -> %d\n",
                 name, ret, first_before, first_after, second_before, second_after,
                 third_before, third_after, fourth_before, fourth_after );
        return -1;
    }

    return 0;
}

static int expect_failed_cqm4_parse_keeps_values( void )
{
    static const char good_text[] = "2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2";
    static const char bad_text[] = "1,1,1x,1,1,1,1,1,1,1,1,1,1,1,1,1";
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( bad_text, &allocation );
    if( !value )
        return -1;

    x264_param_default( &param );
    if( x264_param_parse( &param, "cqm4", good_text ) )
    {
        fprintf( stderr, "baseline parse failed for cqm4=%s\n", good_text );
        free_guarded_string( &allocation );
        return -1;
    }

    int preset_before = param.i_cqm_preset;
    uint8_t cqm_4iy_before[16];
    uint8_t cqm_4py_before[16];
    uint8_t cqm_4ic_before[16];
    uint8_t cqm_4pc_before[16];
    memcpy( cqm_4iy_before, param.cqm_4iy, sizeof(cqm_4iy_before) );
    memcpy( cqm_4py_before, param.cqm_4py, sizeof(cqm_4py_before) );
    memcpy( cqm_4ic_before, param.cqm_4ic, sizeof(cqm_4ic_before) );
    memcpy( cqm_4pc_before, param.cqm_4pc, sizeof(cqm_4pc_before) );

    int ret = x264_param_parse( &param, "cqm4", value );
    free_guarded_string( &allocation );
    if( ret != X264_PARAM_BAD_VALUE ||
        param.i_cqm_preset != preset_before ||
        memcmp( param.cqm_4iy, cqm_4iy_before, sizeof(cqm_4iy_before) ) ||
        memcmp( param.cqm_4py, cqm_4py_before, sizeof(cqm_4py_before) ) ||
        memcmp( param.cqm_4ic, cqm_4ic_before, sizeof(cqm_4ic_before) ) ||
        memcmp( param.cqm_4pc, cqm_4pc_before, sizeof(cqm_4pc_before) ) )
    {
        fprintf( stderr, "guarded failed parse changed cqm4: ret %d preset %d -> %d\n",
                 ret, preset_before, param.i_cqm_preset );
        return -1;
    }

    return 0;
}

static int expect_failed_parse_keeps_two_uint32_values( const char *name, const char *bad_text,
                                                        const char *good_text,
                                                        size_t first_field_offset, size_t second_field_offset )
{
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( bad_text, &allocation );
    if( !value )
        return -1;

    x264_param_default( &param );
    if( x264_param_parse( &param, name, good_text ) )
    {
        fprintf( stderr, "baseline parse failed for %s=%s\n", name, good_text );
        free_guarded_string( &allocation );
        return -1;
    }

    uint32_t *first_field = (uint32_t *)((char *)&param + first_field_offset);
    uint32_t *second_field = (uint32_t *)((char *)&param + second_field_offset);
    uint32_t first_before = *first_field;
    uint32_t second_before = *second_field;
    int ret = x264_param_parse( &param, name, value );
    uint32_t first_after = *first_field;
    uint32_t second_after = *second_field;
    free_guarded_string( &allocation );
    if( ret != X264_PARAM_BAD_VALUE || first_after != first_before || second_after != second_before )
    {
        fprintf( stderr, "guarded failed parse changed %s: ret %d first %u -> %u second %u -> %u\n",
                 name, ret, first_before, first_after, second_before, second_after );
        return -1;
    }

    return 0;
}

static int expect_failed_parse_keeps_two_float_values( const char *name, const char *bad_text,
                                                       const char *good_text,
                                                       size_t first_field_offset, size_t second_field_offset )
{
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( bad_text, &allocation );
    if( !value )
        return -1;

    x264_param_default( &param );
    if( x264_param_parse( &param, name, good_text ) )
    {
        fprintf( stderr, "baseline parse failed for %s=%s\n", name, good_text );
        free_guarded_string( &allocation );
        return -1;
    }

    float *first_field = (float *)((char *)&param + first_field_offset);
    float *second_field = (float *)((char *)&param + second_field_offset);
    float first_before = *first_field;
    float second_before = *second_field;
    int ret = x264_param_parse( &param, name, value );
    float first_after = *first_field;
    float second_after = *second_field;
    free_guarded_string( &allocation );
    if( ret != X264_PARAM_BAD_VALUE || first_after != first_before || second_after != second_before )
    {
        fprintf( stderr, "guarded failed parse changed %s: ret %d first %.9g -> %.9g second %.9g -> %.9g\n",
                 name, ret, first_before, first_after, second_before, second_after );
        return -1;
    }

    return 0;
}

static int expect_failed_parse_keeps_int_float_values( const char *name, const char *bad_text,
                                                       const char *good_text,
                                                       size_t int_field_offset, size_t float_field_offset )
{
    x264_param_t param;
    struct guard_allocation allocation = { 0 };
    char *value = make_guarded_string( bad_text, &allocation );
    if( !value )
        return -1;

    x264_param_default( &param );
    if( x264_param_parse( &param, name, good_text ) )
    {
        fprintf( stderr, "baseline parse failed for %s=%s\n", name, good_text );
        free_guarded_string( &allocation );
        return -1;
    }

    int *int_field = (int *)((char *)&param + int_field_offset);
    float *float_field = (float *)((char *)&param + float_field_offset);
    int int_before = *int_field;
    float float_before = *float_field;
    int ret = x264_param_parse( &param, name, value );
    int int_after = *int_field;
    float float_after = *float_field;
    free_guarded_string( &allocation );
    if( ret != X264_PARAM_BAD_VALUE || int_after != int_before || float_after != float_before )
    {
        fprintf( stderr, "guarded failed parse changed %s: ret %d int %d -> %d float %.9g -> %.9g\n",
                 name, ret, int_before, int_after, float_before, float_after );
        return -1;
    }

    return 0;
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
        { "level", "4.1" },
        { "level-idc", "41" },
        { "avcintra-class", "50" },
        { "chromaloc", "2" },
        { "ref", "3" },
        { "dpb-size", "4" },
        { "keyint", "250" },
        { "keyint", "infinite" },
        { "min-keyint", "25" },
        { "scenecut", "40" },
        { "scenecut", "no" },
        { "bframes", "3" },
        { "b-adapt", "true" },
        { "b-adapt", "2" },
        { "b-bias", "0" },
        { "b-pyramid", "normal" },
        { "b-pyramid", "2" },
        { "fps", "25" },
        { "fps", "2147484" },
        { "fps", "30000/1001" },
        { "threads", "auto" },
        { "threads", "1" },
        { "lookahead-threads", "auto" },
        { "lookahead-threads", "1" },
        { "sync-lookahead", "auto" },
        { "sync-lookahead", "1" },
        { "slices", "1" },
        { "slice-max-size", "1500" },
        { "slice-max-mbs", "1" },
        { "slice-min-mbs", "1" },
        { "slices-max", "1" },
        { "cabac-idc", "1" },
        { "vbv-maxrate", "auto_high" },
        { "vbv-bufsize", "auto_main" },
        { "vbv-init", "0.9" },
        { "aq2-strength", "0.5" },
        { "aq2-sensitivity", "2.0" },
        { "aq2-ifactor", "1.1" },
        { "aq2-pfactor", "1.2" },
        { "aq2-bfactor", "1.3" },
        { "aq-mode", "1" },
        { "aq-strength", "0.8" },
        { "aq-bias-strength", "0.2" },
        { "aq-sensitivity", "10.0" },
        { "aq-ifactor", "1.1" },
        { "aq-pfactor", "1.2" },
        { "aq-bfactor", "1.3" },
        { "aq3-mode", "1" },
        { "aq3-sensitivity", "10.0" },
        { "log-file-level", "info" },
        { "log-file-level", "2" },
        { "log", "2" },
        { "weightp", "2" },
        { "chroma-qp-offset", "-2" },
        { "me-range", "16" },
        { "mv-range", "-1" },
        { "mv-range-thread", "-1" },
        { "subme", "7" },
        { "trellis", "1" },
        { "deadzone-inter", "21" },
        { "deadzone-intra", "11" },
        { "nr", "0" },
        { "bitrate", "1000" },
        { "qp", "22" },
        { "qp-constant", "23" },
        { "crf", "20.5" },
        { "crf-max", "30.5" },
        { "ratetol", "1.0" },
        { "ratetol", "inf" },
        { "vbv-maxrate", "1000" },
        { "vbv-bufsize", "2000" },
        { "ipratio", "1.4" },
        { "ip-factor", "1.5" },
        { "pbratio", "1.3" },
        { "pb-factor", "1.2" },
        { "fade-compensate", "0.1" },
        { "qcomp", "0.6" },
        { "qblur", "0.5" },
        { "cplxblur", "20.0" },
        { "cplx-blur", "21.0" },
        { "fgo", "0" },
        { "sps-id", "0" },
        { "rc-lookahead", "40" },
        { "qpstep", "4" },
        { "pass", "2" },
        { "opts", "3" },
        { "opencl-device", "0" },
        { "frame-packing", "0" },
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
        { "asm", "sse2" },
        { "asm", "sse2,ssse3" },
#endif
    };
    static const struct
    {
        const char *name;
        const char *value;
    } bad_cases[] = {
        { "threads", "1x" },
        { "lookahead-threads", "1x" },
        { "sync-lookahead", "1x" },
        { "fps", "25/1x" },
        { "fps", "25x" },
        { "fps", "25/0" },
        { "fps", "nan" },
        { "fps", "inf" },
        { "vbv-init", "nan" },
        { "vbv-init", "inf" },
        { "aq2-strength", "inf" },
        { "aq2-sensitivity", "2.0x" },
        { "aq2-ifactor", "1.1x" },
        { "aq2-pfactor", "1.2x" },
        { "aq2-bfactor", "1.3x" },
        { "aq-mode", "1x" },
        { "aq-strength", "0.8x" },
        { "aq-strength", "inf" },
        { "aq-bias-strength", "0.2x" },
        { "aq-bias-strength", "nan" },
        { "aq-sensitivity", "10.0x" },
        { "aq-ifactor", "1.1x" },
        { "aq-pfactor", "1.2x" },
        { "aq-bfactor", "1.3x" },
        { "aq3-sensitivity", "10.0x" },
        { "aq3-sensitivity", "inf" },
        { "opencl-device", "0x" },
        { "level", "4.1x" },
        { "level", "nan" },
        { "level", "inf" },
        { "avcintra-class", "50x" },
        { "chromaloc", "2x" },
        { "chromaloc", "6" },
        { "ref", "3x" },
        { "dpb-size", "4x" },
        { "keyint", "250x" },
        { "min-keyint", "25x" },
        { "scenecut", "40x" },
        { "bframes", "3x" },
        { "b-adapt", "2x" },
        { "b-bias", "0x" },
        { "b-pyramid", "2x" },
        { "b-pyramid", "normalx" },
        { "slice-max-size", "1x" },
        { "slice-max-mbs", "1x" },
        { "slice-min-mbs", "1x" },
        { "slices", "1x" },
        { "slices-max", "1x" },
        { "aq3-mode", "1x" },
        { "frame-packing", "0x" },
        { "log", "2x" },
        { "log-file-level", "infox" },
        { "cabac-idc", "1x" },
        { "weightp", "2x" },
        { "chroma-qp-offset", "-2x" },
        { "me-range", "16x" },
        { "mv-range", "-1x" },
        { "mv-range-thread", "-1x" },
        { "subme", "7x" },
        { "trellis", "1x" },
        { "deadzone-inter", "21x" },
        { "deadzone-intra", "11x" },
        { "nr", "0x" },
        { "bitrate", "1000x" },
        { "qp", "22x" },
        { "qp-constant", "23x" },
        { "crf", "20.5x" },
        { "crf", "nan" },
        { "crf", "inf" },
        { "crf-max", "30.5x" },
        { "crf-max", "nan" },
        { "crf-max", "inf" },
        { "ratetol", "1.0x" },
        { "ratetol", "nan" },
        { "ratetol", "infjunk" },
        { "vbv-maxrate", "1000x" },
        { "vbv-bufsize", "2000x" },
        { "ipratio", "1.4x" },
        { "ipratio", "nan" },
        { "ip-factor", "inf" },
        { "pbratio", "1.3x" },
        { "pbratio", "nan" },
        { "pb-factor", "inf" },
        { "fade-compensate", "0.1x" },
        { "fade-compensate", "nan" },
        { "qcomp", "0.6x" },
        { "qcomp", "inf" },
        { "qblur", "0.5x" },
        { "qblur", "nan" },
        { "cplxblur", "20.0x" },
        { "cplx-blur", "inf" },
        { "fgo", "0x" },
        { "sps-id", "0x" },
        { "rc-lookahead", "40x" },
        { "qpstep", "4x" },
        { "pass", "2x" },
        { "opts", "4" },
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
        { "asm", "sse2,,ssse3" },
        { "asm", "sse2," },
#endif
    };

    for( size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++ )
        if( expect_guarded_value( cases[i].name, cases[i].value, 0 ) )
            return 1;

    for( size_t i = 0; i < sizeof(bad_cases) / sizeof(bad_cases[0]); i++ )
        if( expect_guarded_value( bad_cases[i].name, bad_cases[i].value, X264_PARAM_BAD_VALUE ) )
            return 1;

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    if( expect_unchanged_uint32_value( "asm", "sse2,,ssse3", "sse2", offsetof( x264_param_t, cpu ) ) )
        return 1;
#endif
    if( expect_apply_profile_failure_keeps_values() )
        return 1;
    if( expect_failed_mastering_display_parse_keeps_values() )
        return 1;
    if( expect_failed_cqm4_parse_keeps_values() )
        return 1;

    if( expect_unchanged_int_value( "threads", "1x", "2", offsetof( x264_param_t, i_threads ) ) ||
        expect_unchanged_int_value( "lookahead-threads", "1x", "2", offsetof( x264_param_t, i_lookahead_threads ) ) ||
        expect_unchanged_int_value( "sync-lookahead", "1x", "2", offsetof( x264_param_t, i_sync_lookahead ) ) ||
        expect_unchanged_int_value( "slice-max-size", "1x", "1500", offsetof( x264_param_t, i_slice_max_size ) ) ||
        expect_unchanged_int_value( "slice-max-mbs", "1x", "2", offsetof( x264_param_t, i_slice_max_mbs ) ) ||
        expect_unchanged_int_value( "slice-min-mbs", "1x", "2", offsetof( x264_param_t, i_slice_min_mbs ) ) ||
        expect_unchanged_int_value( "slices", "1x", "2", offsetof( x264_param_t, i_slice_count ) ) ||
        expect_unchanged_int_value( "slices-max", "1x", "2", offsetof( x264_param_t, i_slice_count_max ) ) ||
        expect_unchanged_int_value( "aq3-mode", "1x", "1", offsetof( x264_param_t, rc.i_aq3_mode ) ) ||
        expect_unchanged_int_value( "log", "2x", "2", offsetof( x264_param_t, i_log_level ) ) ||
        expect_unchanged_int_value( "log-file-level", "2x", "2", offsetof( x264_param_t, i_log_file_level ) ) ||
        expect_unchanged_int_value( "opencl-device", "0x", "1", offsetof( x264_param_t, i_opencl_device ) ) ||
        expect_unchanged_int_value( "frame-packing", "0x", "1", offsetof( x264_param_t, i_frame_packing ) ) ||
        expect_unchanged_int_value( "level", "4.1x", "4.1", offsetof( x264_param_t, i_level_idc ) ) ||
        expect_failed_parse_keeps_two_uint32_values( "fps", "25/0", "30000/1001",
                                                     offsetof( x264_param_t, i_fps_num ),
                                                     offsetof( x264_param_t, i_fps_den ) ) ||
        expect_unchanged_int_value( "avcintra-class", "50x", "50", offsetof( x264_param_t, i_avcintra_class ) ) ||
        expect_unchanged_int_value( "chromaloc", "2x", "2", offsetof( x264_param_t, vui.i_chroma_loc ) ) ||
        expect_unchanged_int_value( "chromaloc", "6", "2", offsetof( x264_param_t, vui.i_chroma_loc ) ) ||
        expect_unchanged_int_value( "ref", "3x", "3", offsetof( x264_param_t, i_frame_reference ) ) ||
        expect_unchanged_int_value( "dpb-size", "4x", "4", offsetof( x264_param_t, i_dpb_size ) ) ||
        expect_unchanged_int_value( "keyint", "250x", "250", offsetof( x264_param_t, i_keyint_max ) ) ||
        expect_unchanged_int_value( "scenecut", "40x", "40", offsetof( x264_param_t, i_scenecut_threshold ) ) ||
        expect_unchanged_int_value( "bframes", "3x", "3", offsetof( x264_param_t, i_bframe ) ) ||
        expect_unchanged_int_value( "b-adapt", "2x", "2", offsetof( x264_param_t, i_bframe_adaptive ) ) ||
        expect_unchanged_int_value( "b-bias", "0x", "0", offsetof( x264_param_t, i_bframe_bias ) ) ||
        expect_unchanged_int_value( "b-pyramid", "2x", "2", offsetof( x264_param_t, i_bframe_pyramid ) ) ||
        expect_unchanged_int_value( "b-pyramid", "normalx", "2", offsetof( x264_param_t, i_bframe_pyramid ) ) ||
        expect_unchanged_int_value( "cabac-idc", "1x", "1", offsetof( x264_param_t, i_cabac_init_idc ) ) ||
        expect_failed_parse_keeps_three_int_values( "deblock", "1:1x", "1:1",
                                                    offsetof( x264_param_t, b_deblocking_filter ),
                                                    offsetof( x264_param_t, i_deblocking_filter_alphac0 ),
                                                    offsetof( x264_param_t, i_deblocking_filter_beta ) ) ||
        expect_failed_parse_keeps_three_int_values( "deblock", "foo", "1:1",
                                                    offsetof( x264_param_t, b_deblocking_filter ),
                                                    offsetof( x264_param_t, i_deblocking_filter_alphac0 ),
                                                    offsetof( x264_param_t, i_deblocking_filter_beta ) ) ||
        expect_unchanged_int_value( "weightp", "2x", "2", offsetof( x264_param_t, analyse.i_weighted_pred ) ) ||
        expect_unchanged_int_value( "chroma-qp-offset", "-2x", "-2", offsetof( x264_param_t, analyse.i_chroma_qp_offset ) ) ||
        expect_unchanged_int_value( "me-range", "16x", "16", offsetof( x264_param_t, analyse.i_me_range ) ) ||
        expect_unchanged_int_value( "mv-range", "-1x", "-1", offsetof( x264_param_t, analyse.i_mv_range ) ) ||
        expect_unchanged_int_value( "mv-range-thread", "-1x", "-1", offsetof( x264_param_t, analyse.i_mv_range_thread ) ) ||
        expect_unchanged_int_value( "subme", "7x", "7", offsetof( x264_param_t, analyse.i_subpel_refine ) ) ||
        expect_failed_parse_keeps_two_float_values( "psy-rd", "1.0:0.2x", "1.0:0.2",
                                                    offsetof( x264_param_t, analyse.f_psy_rd ),
                                                    offsetof( x264_param_t, analyse.f_psy_trellis ) ) ||
        expect_unchanged_int_value( "trellis", "1x", "1", offsetof( x264_param_t, analyse.i_trellis ) ) ||
        expect_unchanged_int_value( "deadzone-inter", "21x", "21", offsetof( x264_param_t, analyse.i_luma_deadzone[0] ) ) ||
        expect_unchanged_int_value( "deadzone-intra", "11x", "11", offsetof( x264_param_t, analyse.i_luma_deadzone[1] ) ) ||
        expect_unchanged_int_value( "nr", "0x", "0", offsetof( x264_param_t, analyse.i_noise_reduction ) ) ||
        expect_failed_parse_keeps_two_int_values( "bitrate", "1000x", "1000",
                                                  offsetof( x264_param_t, rc.i_rc_method ),
                                                  offsetof( x264_param_t, rc.i_bitrate ) ) ||
        expect_failed_parse_keeps_two_int_values( "qp", "22x", "22",
                                                  offsetof( x264_param_t, rc.i_rc_method ),
                                                  offsetof( x264_param_t, rc.i_qp_constant ) ) ||
        expect_failed_parse_keeps_int_float_values( "crf", "20.5x", "20.5",
                                                    offsetof( x264_param_t, rc.i_rc_method ),
                                                    offsetof( x264_param_t, rc.f_rf_constant ) ) ||
        expect_unchanged_float_value( "crf-max", "30.5x", "30.5", offsetof( x264_param_t, rc.f_rf_constant_max ) ) ||
        expect_unchanged_float_value( "ratetol", "1.0x", "1.0", offsetof( x264_param_t, rc.f_rate_tolerance ) ) ||
        expect_unchanged_int_value( "vbv-maxrate", "1000x", "1000", offsetof( x264_param_t, rc.i_vbv_max_bitrate ) ) ||
        expect_unchanged_int_value( "vbv-bufsize", "2000x", "2000", offsetof( x264_param_t, rc.i_vbv_buffer_size ) ) ||
        expect_unchanged_float_value( "ipratio", "1.4x", "1.4", offsetof( x264_param_t, rc.f_ip_factor ) ) ||
        expect_unchanged_float_value( "pbratio", "1.3x", "1.3", offsetof( x264_param_t, rc.f_pb_factor ) ) ||
        expect_unchanged_int_value( "aq-mode", "1x", "1", offsetof( x264_param_t, rc.i_aq_mode ) ) ||
        expect_unchanged_float_value( "aq-strength", "0.8x", "0.8", offsetof( x264_param_t, rc.f_aq_strength ) ) ||
        expect_unchanged_float_value( "aq-bias-strength", "0.2x", "0.2", offsetof( x264_param_t, rc.f_aq_bias_strength ) ) ||
        expect_unchanged_float_value( "aq-sensitivity", "10.0x", "10.0", offsetof( x264_param_t, rc.f_aq_sensitivity ) ) ||
        expect_unchanged_float_value( "aq-ifactor", "1.1x", "1.1", offsetof( x264_param_t, rc.f_aq_ifactor ) ) ||
        expect_unchanged_float_value( "aq-pfactor", "1.2x", "1.2", offsetof( x264_param_t, rc.f_aq_pfactor ) ) ||
        expect_unchanged_float_value( "aq-bfactor", "1.3x", "1.3", offsetof( x264_param_t, rc.f_aq_bfactor ) ) ||
        expect_unchanged_float_value( "aq3-sensitivity", "10.0x", "10.0", offsetof( x264_param_t, rc.f_aq3_sensitivity ) ) ||
        expect_failed_parse_keeps_two_float_values( "aq3-ifactor", "1.1:1.2x", "1.1:1.2",
                                                    offsetof( x264_param_t, rc.f_aq3_ifactor[0] ),
                                                    offsetof( x264_param_t, rc.f_aq3_ifactor[1] ) ) ||
        expect_failed_parse_keeps_two_float_values( "aq3-pfactor", "1.3:1.4x", "1.3:1.4",
                                                    offsetof( x264_param_t, rc.f_aq3_pfactor[0] ),
                                                    offsetof( x264_param_t, rc.f_aq3_pfactor[1] ) ) ||
        expect_failed_parse_keeps_two_float_values( "aq3-bfactor", "1.5:1.6x", "1.5:1.6",
                                                    offsetof( x264_param_t, rc.f_aq3_bfactor[0] ),
                                                    offsetof( x264_param_t, rc.f_aq3_bfactor[1] ) ) ||
        expect_failed_parse_keeps_four_int_values( "aq3-boundary", "192:64x24", "192:64:24",
                                                   offsetof( x264_param_t, rc.b_aq3_boundary ),
                                                   offsetof( x264_param_t, rc.i_aq3_boundary[0] ),
                                                   offsetof( x264_param_t, rc.i_aq3_boundary[1] ),
                                                   offsetof( x264_param_t, rc.i_aq3_boundary[2] ) ) ||
        expect_unchanged_float_value( "fade-compensate", "0.1x", "0.1", offsetof( x264_param_t, rc.f_fade_compensate ) ) ||
        expect_unchanged_float_value( "qcomp", "0.6x", "0.6", offsetof( x264_param_t, rc.f_qcompress ) ) ||
        expect_unchanged_float_value( "qblur", "0.5x", "0.5", offsetof( x264_param_t, rc.f_qblur ) ) ||
        expect_unchanged_float_value( "cplxblur", "20.0x", "20.0", offsetof( x264_param_t, rc.f_complexity_blur ) ) ||
        expect_unchanged_int_value( "fgo", "0x", "0", offsetof( x264_param_t, analyse.i_fgo ) ) ||
        expect_unchanged_int_value( "sps-id", "0x", "0", offsetof( x264_param_t, i_sps_id ) ) ||
        expect_unchanged_int_value( "rc-lookahead", "40x", "40", offsetof( x264_param_t, rc.i_lookahead ) ) ||
        expect_unchanged_int_value( "qpstep", "4x", "4", offsetof( x264_param_t, rc.i_qp_step ) ) ||
        expect_failed_parse_keeps_two_int_values( "min-keyint", "25x", "25",
                                                  offsetof( x264_param_t, i_keyint_min ),
                                                  offsetof( x264_param_t, i_keyint_max ) ) ||
        expect_failed_parse_keeps_two_int_values( "pass", "2x", "2",
                                                  offsetof( x264_param_t, rc.b_stat_write ),
                                                  offsetof( x264_param_t, rc.b_stat_read ) ) ||
        expect_unchanged_int_value( "opts", "4", "3", offsetof( x264_param_t, i_opts_write ) ) ||
        expect_unchanged_float_value( "vbv-init", "0.5x", "0.5", offsetof( x264_param_t, rc.f_vbv_buffer_init ) ) ||
        expect_unchanged_float_value( "aq2-strength", "0.5x", "0.5", offsetof( x264_param_t, rc.f_aq2_strength ) ) ||
        expect_unchanged_float_value( "aq2-sensitivity", "2.0x", "2.0", offsetof( x264_param_t, rc.f_aq2_sensitivity ) ) ||
        expect_unchanged_float_value( "aq2-ifactor", "1.1x", "1.1", offsetof( x264_param_t, rc.f_aq2_ifactor ) ) ||
        expect_unchanged_float_value( "aq2-pfactor", "1.2x", "1.2", offsetof( x264_param_t, rc.f_aq2_pfactor ) ) ||
        expect_unchanged_float_value( "aq2-bfactor", "1.3x", "1.3", offsetof( x264_param_t, rc.f_aq2_bfactor ) ) ||
        expect_failed_parse_keeps_int_default( "aq2-strength", "0.5x", offsetof( x264_param_t, rc.b_aq2 ) ) )
        return 1;

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

run_audio_avs_object_smoke()
{
    avs_object_log=$smoke_dir/audio-avs-object.log
    if ! ( cd "$smoke_dir" && make -j"$make_jobs" input/audio/avs.o ) >"$avs_object_log" 2>&1; then
        printf '%s\n' "failed to build input/audio/avs.c object smoke: $avs_object_log" >&2
        exit 1
    fi
}

run_audio_faac_stub_object_smoke()
{
    faac_object_log=$smoke_dir/audio-faac-stub-object.log
    faac_object=$smoke_dir/audio/encoders/enc_faac-stub.o
    faac_stub_dir=$smoke_dir/faac-stub

    mkdir -p "$faac_stub_dir" "$smoke_dir/audio/encoders"
    cat > "$faac_stub_dir/faac.h" <<'FAAC_H'
#ifndef FAAC_H
#define FAAC_H

#include <stdint.h>

#define LOW 2
#define MPEG4 1
#define FAAC_INPUT_FLOAT 1
#define SHORTCTL_NORMAL 0
#define SHORTCTL_NOSHORT 1
#define SHORTCTL_NOLONG 2

typedef void *faacEncHandle;

typedef struct faacEncConfiguration
{
    int aacObjectType;
    int mpegVersion;
    int useTns;
    int shortctl;
    int allowMidside;
    int bandWidth;
    int outputFormat;
    int inputFormat;
    int quantqual;
    unsigned long bitRate;
    int useLfe;
    int channel_map[8];
} faacEncConfiguration, *faacEncConfigurationPtr;

faacEncHandle faacEncOpen( unsigned long sampleRate, unsigned int numChannels,
                           unsigned long *inputSamples, unsigned long *maxOutputBytes );
faacEncConfigurationPtr faacEncGetCurrentConfiguration( faacEncHandle hEncoder );
int faacEncSetConfiguration( faacEncHandle hEncoder, faacEncConfigurationPtr config );
void faacEncGetDecoderSpecificInfo( faacEncHandle hEncoder, unsigned char **buffer, unsigned long *bufferSize );
int faacEncEncode( faacEncHandle hEncoder, int32_t *inputBuffer, unsigned int samplesInput,
                   unsigned char *outputBuffer, unsigned int bufferSize );
int faacEncClose( faacEncHandle hEncoder );

#endif
FAAC_H

    if ! ${CC:-cc} -std=gnu17 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200112L \
        -Wall -Wextra -Werror -Wno-unused-parameter \
        -I"$faac_stub_dir" -I"$smoke_dir" -I"$root" \
        -DHIGH_BIT_DEPTH=0 -DBIT_DEPTH=8 \
        -c "$root/audio/encoders/enc_faac.c" -o "$faac_object" >"$faac_object_log" 2>&1; then
        printf '%s\n' "failed to build audio/encoders/enc_faac.c stub object smoke: $faac_object_log" >&2
        exit 1
    fi
}

run_audio_lavc_object_smoke()
{
    lavc_object_log=$smoke_dir/audio-lavc-object.log
    lavc_object=$smoke_dir/audio/encoders/enc_lavc.o

    if ! PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-$msys2_pkg_config_path} \
        pkg-config --exists libavcodec libavutil libswresample; then
        printf '%s\n' "audio LAVC object smoke skipped: missing libavcodec/libavutil/libswresample" >"$lavc_object_log"
        printf '%s\n' "audio LAVC object smoke skipped: missing libavcodec/libavutil/libswresample"
        return 0
    fi

    mkdir -p "$smoke_dir/audio/encoders"
    if ! ${CC:-cc} -std=gnu17 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200112L \
        -Wall -Wextra -Werror -Wno-unused-parameter \
        -Wno-missing-field-initializers -Wno-sign-compare \
        -I"$smoke_dir" -I"$root" \
        -DHIGH_BIT_DEPTH=0 -DBIT_DEPTH=8 \
        $(PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-$msys2_pkg_config_path} pkg-config --cflags libavcodec libavutil libswresample) \
        -c "$root/audio/encoders/enc_lavc.c" -o "$lavc_object" >"$lavc_object_log" 2>&1; then
        printf '%s\n' "failed to build audio/encoders/enc_lavc.c object smoke: $lavc_object_log" >&2
        exit 1
    fi
    printf '%s\n' "audio LAVC object smoke built: $lavc_object" >"$lavc_object_log"
}

run_filter_option_helper_smoke()
{
    helper_source=$smoke_dir/filter-option-helper-smoke.c
    helper_binary=$smoke_dir/filter-option-helper-smoke$exe
    helper_log=$smoke_dir/filter-option-helper-smoke.log

    cat > "$helper_source" <<'HELPER_C'
#include "filters/filters.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void x264_cli_log( const char *name, int i_level, const char *fmt, ... )
{
    (void)name;
    (void)i_level;
    va_list args;
    va_start( args, fmt );
    vfprintf( stderr, fmt, args );
    va_end( args );
}

static int expect_float_success( const char *arg, double expected )
{
    double value = -99.0;
    if( x264_otof_checked( arg, &value ) )
        return 1;
    return value != expected;
}

static int expect_float_failure( const char *arg )
{
    double value = 123.0;
    return !x264_otof_checked( arg, &value ) || value != 123.0;
}

static int expect_float_default( const char *arg )
{
    return x264_otof( arg, 7.0 ) != 7.0;
}

static int expect_int_success( const char *arg, int expected )
{
    int value = -99;
    if( x264_otoi_checked( arg, &value ) )
        return 1;
    return value != expected;
}

static int expect_int_failure( const char *arg )
{
    int value = 123;
    return !x264_otoi_checked( arg, &value ) || value != 123;
}

static int expect_int_default( const char *arg )
{
    return x264_otoi( arg, 7 ) != 7;
}

int main( void )
{
    return expect_float_success( "1.25", 1.25 ) ||
           expect_float_success( ".5", .5 ) ||
           expect_float_success( "-1.25", -1.25 ) ||
           expect_float_failure( "+1.25" ) ||
           expect_float_failure( " 1.25" ) ||
           expect_float_failure( "- 1.25" ) ||
           expect_float_failure( "1.25x" ) ||
           expect_float_default( "+1.25" ) ||
           expect_float_default( " 1.25" ) ||
           expect_int_success( "1", 1 ) ||
           expect_int_success( "-1", -1 ) ||
           expect_int_success( "0x10", 16 ) ||
           expect_int_failure( "+1" ) ||
           expect_int_failure( " 1" ) ||
           expect_int_failure( "- 1" ) ||
           expect_int_failure( "1x" ) ||
           expect_int_default( "+1" ) ||
           expect_int_default( " 1" );
}
HELPER_C

    if ! ${CC:-cc} -std=gnu17 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200112L \
        -Wall -Wextra -Werror -Wno-unused-parameter \
        -I"$smoke_dir" -I"$root" \
        -DHIGH_BIT_DEPTH=0 -DBIT_DEPTH=8 \
        "$helper_source" "$smoke_dir/filters/filters.o" -o "$helper_binary" >"$helper_log" 2>&1; then
        printf '%s\n' "failed to build filter option helper smoke: $helper_log" >&2
        exit 1
    fi
    if ! "$helper_binary" >>"$helper_log" 2>&1; then
        printf '%s\n' "filter option helper smoke failed: $helper_log" >&2
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
    y4m_tail_fps=$smoke_dir/smoke-tail-fps.y4m
    y4m_tail_fps_out=$smoke_dir/smoke-tail-fps.264
    y4m_mono=$smoke_dir/smoke-mono.y4m
    y4m_mono_out=$smoke_dir/smoke-mono.264
    y4m_color_range_tail=$smoke_dir/smoke-color-range-tail.y4m
    y4m_color_range_tail_out=$smoke_dir/smoke-color-range-tail.264
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
    raw_bad_prefix_out=$smoke_dir/smoke-raw-bad-prefix.264
    raw_bad_prefix_log=$smoke_dir/smoke-raw-bad-prefix.log
    example_bin=$smoke_dir/example$exe
    example_out=$smoke_dir/example.264
    example_bad_res_out=$smoke_dir/example-bad-res.264
    example_bad_res_log=$smoke_dir/example-bad-res.log
    example_bad_overflow_out=$smoke_dir/example-bad-overflow.264
    example_bad_overflow_log=$smoke_dir/example-bad-overflow.log
    example_bad_prefix_out=$smoke_dir/example-bad-prefix.264
    example_bad_prefix_log=$smoke_dir/example-bad-prefix.log
    param_bad_fps_out=$smoke_dir/smoke-param-bad-fps.264
    param_bad_fps_log=$smoke_dir/smoke-param-bad-fps.log
    param_bad_scalar_prefix_out=$smoke_dir/smoke-param-bad-scalar-prefix.264
    param_bad_scalar_prefix_log=$smoke_dir/smoke-param-bad-scalar-prefix.log
    preset_numeric_out=$smoke_dir/smoke-preset-numeric.264
    preset_bad_prefix_out=$smoke_dir/smoke-preset-bad-prefix.264
    preset_bad_prefix_log=$smoke_dir/smoke-preset-bad-prefix.log
    cli_bad_int_prefix_out=$smoke_dir/smoke-cli-bad-int-prefix.264
    cli_bad_int_prefix_log=$smoke_dir/smoke-cli-bad-int-prefix.log
    cli_bad_uint_prefix_out=$smoke_dir/smoke-cli-bad-uint-prefix.264
    cli_bad_uint_prefix_log=$smoke_dir/smoke-cli-bad-uint-prefix.log
    cli_bad_display_prefix_out=$smoke_dir/smoke-cli-bad-display-prefix.264
    cli_bad_display_prefix_log=$smoke_dir/smoke-cli-bad-display-prefix.log
    param_partitions_out=$smoke_dir/smoke-param-partitions.264
    param_bad_partitions_out=$smoke_dir/smoke-param-bad-partitions.264
    param_bad_partitions_log=$smoke_dir/smoke-param-bad-partitions.log
    param_bad_analyse_out=$smoke_dir/smoke-param-bad-analyse.264
    param_bad_analyse_log=$smoke_dir/smoke-param-bad-analyse.log
    param_list_space_out=$smoke_dir/smoke-param-list-space.264
    param_bad_list_prefix_out=$smoke_dir/smoke-param-bad-list-prefix.264
    param_bad_list_prefix_log=$smoke_dir/smoke-param-bad-list-prefix.log
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
    param_cqm4_space_out=$smoke_dir/smoke-param-cqm4-space.264
    param_bad_cqm4_out=$smoke_dir/smoke-param-bad-cqm4.264
    param_bad_cqm4_log=$smoke_dir/smoke-param-bad-cqm4.log
    cli_bad_float_prefix_out=$smoke_dir/smoke-cli-bad-float-prefix.264
    cli_bad_float_prefix_log=$smoke_dir/smoke-cli-bad-float-prefix.log
    filter_bad_pad_out=$smoke_dir/smoke-filter-bad-pad.264
    filter_bad_pad_log=$smoke_dir/smoke-filter-bad-pad.log
    filter_bad_crop_out=$smoke_dir/smoke-filter-bad-crop.264
    filter_bad_crop_log=$smoke_dir/smoke-filter-bad-crop.log
    filter_bad_depth_out=$smoke_dir/smoke-filter-bad-depth.264
    filter_bad_depth_log=$smoke_dir/smoke-filter-bad-depth.log
    filter_bad_select_every_out=$smoke_dir/smoke-filter-bad-select-every.264
    filter_bad_select_every_log=$smoke_dir/smoke-filter-bad-select-every.log
    filter_empty_select_every_out=$smoke_dir/smoke-filter-empty-select-every.264
    filter_empty_select_every_log=$smoke_dir/smoke-filter-empty-select-every.log
    filter_hqdn3d_out=$smoke_dir/smoke-filter-hqdn3d.264
    filter_bad_hqdn3d_out=$smoke_dir/smoke-filter-bad-hqdn3d.264
    filter_bad_hqdn3d_log=$smoke_dir/smoke-filter-bad-hqdn3d.log
    filter_bad_yadif_out=$smoke_dir/smoke-filter-bad-yadif.264
    filter_bad_yadif_log=$smoke_dir/smoke-filter-bad-yadif.log
    filter_bad_resize_width_out=$smoke_dir/smoke-filter-bad-resize-width.264
    filter_bad_resize_width_log=$smoke_dir/smoke-filter-bad-resize-width.log
    filter_bad_resize_depth_out=$smoke_dir/smoke-filter-bad-resize-depth.264
    filter_bad_resize_depth_log=$smoke_dir/smoke-filter-bad-resize-depth.log
    filter_bad_resize_sar_out=$smoke_dir/smoke-filter-bad-resize-sar.264
    filter_bad_resize_sar_log=$smoke_dir/smoke-filter-bad-resize-sar.log
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
    cqmfile_short=$smoke_dir/smoke-short-cqmfile.cfg
    cqmfile_short_out=$smoke_dir/smoke-short-cqmfile.264
    cqmfile_short_log=$smoke_dir/smoke-short-cqmfile.log
    cqmfile_empty=$smoke_dir/smoke-empty-cqmfile.cfg
    cqmfile_empty_out=$smoke_dir/smoke-empty-cqmfile.264
    cqmfile_empty_log=$smoke_dir/smoke-empty-cqmfile.log
    cqmfile_large=$smoke_dir/smoke-large-cqmfile.cfg
    cqmfile_large_out=$smoke_dir/smoke-large-cqmfile.264
    cqmfile_large_log=$smoke_dir/smoke-large-cqmfile.log
    cqmfile_prefixed=$smoke_dir/smoke-prefixed-cqmfile.cfg
    cqmfile_prefixed_out=$smoke_dir/smoke-prefixed-cqmfile.264
    cqmfile_prefixed_log=$smoke_dir/smoke-prefixed-cqmfile.log
    tc_bad_header=$smoke_dir/smoke-bad-header.tc
    tc_bad_header_out=$smoke_dir/smoke-bad-header.264
    tc_bad_header_log=$smoke_dir/smoke-bad-header.log
    tc_bad_tdecimate=$smoke_dir/smoke-bad-tdecimate.tc
    tc_bad_tdecimate_out=$smoke_dir/smoke-bad-tdecimate.264
    tc_bad_tdecimate_log=$smoke_dir/smoke-bad-tdecimate.log
    tc_tdecimate=$smoke_dir/smoke-tdecimate.tc
    tc_tdecimate_out=$smoke_dir/smoke-tdecimate.264
    tc_bad_double=$smoke_dir/smoke-bad-double.tc
    tc_bad_double_out=$smoke_dir/smoke-bad-double.264
    tc_bad_double_log=$smoke_dir/smoke-bad-double.log
    tc_timebase=$smoke_dir/smoke-timebase.tc
    tc_bad_timebase_out=$smoke_dir/smoke-bad-timebase.264
    tc_bad_timebase_log=$smoke_dir/smoke-bad-timebase.log
    stats_y4m=$smoke_dir/twopass.y4m
    stats_valid=$smoke_dir/twopass.stats
    stats_pass1_out=$smoke_dir/twopass-pass1.264
    stats_bad_resolution=$smoke_dir/twopass-bad-resolution.stats
    stats_bad_resolution_out=$smoke_dir/twopass-bad-resolution.264
    stats_bad_resolution_log=$smoke_dir/twopass-bad-resolution.log
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
    stats_bad_delim=$smoke_dir/twopass-bad-delim.stats
    stats_bad_delim_out=$smoke_dir/twopass-bad-delim.264
    stats_bad_delim_log=$smoke_dir/twopass-bad-delim.log
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
    zone_prefixed_out=$smoke_dir/zone-prefixed.264
    zone_prefixed_log=$smoke_dir/zone-prefixed.log
    zone_empty_opt_out=$smoke_dir/zone-empty-opt.264
    zone_empty_opt_log=$smoke_dir/zone-empty-opt.log
    device_empty_out=$smoke_dir/device-empty.264
    device_empty_log=$smoke_dir/device-empty.log
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
    "$smoke_bin" --demuxer y4m --frames 1 --preset 1 --crf 30 -o "$preset_numeric_out" "$y4m" >/dev/null
    [ -s "$preset_numeric_out" ] || { printf '%s\n' "missing numeric preset smoke output: $preset_numeric_out (input: $y4m)" >&2; exit 1; }
    for bad_preset_prefix in '+1' ' 1'; do
        rm -f "$preset_bad_prefix_log" "$preset_bad_prefix_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --preset "$bad_preset_prefix" --crf 30 -o "$preset_bad_prefix_out" "$y4m" >"$preset_bad_prefix_log" 2>&1; then
            printf '%s\n' "accepted prefixed numeric preset '$bad_preset_prefix': $preset_bad_prefix_out" >&2
            exit 1
        fi
        grep -q "invalid preset" "$preset_bad_prefix_log" ||
        {
            printf '%s\n' "missing prefixed numeric preset parse error for '$bad_preset_prefix' in $preset_bad_prefix_log" >&2
            exit 1
        }
    done
    run_param_list_guard_smoke
    run_audio_avs_object_smoke
    run_audio_faac_stub_object_smoke
    run_audio_lavc_object_smoke
    run_filter_option_helper_smoke
    dd if=/dev/zero of="$raw" bs=384 count=1 2>/dev/null
    "$smoke_bin" --demuxer raw --input-res 16x16 --fps 25 --frames 1 --crf 30 -o "$raw_out" "$raw" >/dev/null
    [ -s "$raw_out" ] || { printf '%s\n' "missing raw smoke output: $raw_out (input: $raw)" >&2; exit 1; }
    if ! ( cd "$smoke_dir" && make -j"$make_jobs" example ) >/dev/null 2>&1; then
        printf '%s\n' "failed to build example smoke binary: $example_bin" >&2
        exit 1
    fi
    [ -x "$example_bin" ] || { printf '%s\n' "missing example smoke binary: $example_bin" >&2; exit 1; }
    "$example_bin" 16x16 < "$raw" > "$example_out"
    [ -s "$example_out" ] || { printf '%s\n' "missing example smoke output: $example_out (input: $raw)" >&2; exit 1; }
    rm -f "$example_bad_res_log" "$example_bad_res_out"
    if "$example_bin" 16x16x < "$raw" >"$example_bad_res_out" 2>"$example_bad_res_log"; then
        printf '%s\n' "accepted example resolution trailing junk: $example_bad_res_out" >&2
        exit 1
    fi
    grep -q "resolution not specified or incorrect" "$example_bad_res_log" ||
    {
        printf '%s\n' "missing example trailing-junk resolution parse error in $example_bad_res_log" >&2
        exit 1
    }
    rm -f "$example_bad_overflow_log" "$example_bad_overflow_out"
    if "$example_bin" 50000x50000 < "$raw" >"$example_bad_overflow_out" 2>"$example_bad_overflow_log"; then
        printf '%s\n' "accepted example overflow resolution: $example_bad_overflow_out" >&2
        exit 1
    fi
    grep -q "resolution not specified or incorrect" "$example_bad_overflow_log" ||
    {
        printf '%s\n' "missing example overflow resolution parse error in $example_bad_overflow_log" >&2
        exit 1
    }
    rm -f "$example_bad_prefix_log" "$example_bad_prefix_out"
    if "$example_bin" +16x16 < "$raw" >"$example_bad_prefix_out" 2>"$example_bad_prefix_log"; then
        printf '%s\n' "accepted example resolution signed prefix: $example_bad_prefix_out" >&2
        exit 1
    fi
    grep -q "resolution not specified or incorrect" "$example_bad_prefix_log" ||
    {
        printf '%s\n' "missing example signed-prefix resolution parse error in $example_bad_prefix_log" >&2
        exit 1
    }
    rm -f "$example_bad_prefix_log" "$example_bad_prefix_out"
    if "$example_bin" ' 16x16' < "$raw" >"$example_bad_prefix_out" 2>"$example_bad_prefix_log"; then
        printf '%s\n' "accepted example resolution leading space: $example_bad_prefix_out" >&2
        exit 1
    fi
    grep -q "resolution not specified or incorrect" "$example_bad_prefix_log" ||
    {
        printf '%s\n' "missing example leading-space resolution parse error in $example_bad_prefix_log" >&2
        exit 1
    }
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
    rm -f "$raw_bad_prefix_log" "$raw_bad_prefix_out"
    if "$smoke_bin" --demuxer raw --input-res +16x16 --fps 25 --frames 1 --crf 30 -o "$raw_bad_prefix_out" "$raw" >"$raw_bad_prefix_log" 2>&1; then
        printf '%s\n' "accepted raw input resolution signed prefix: $raw_bad_prefix_out" >&2
        exit 1
    fi
    grep -q "invalid resolution" "$raw_bad_prefix_log" ||
    {
        printf '%s\n' "missing raw signed-prefix resolution parse error in $raw_bad_prefix_log" >&2
        exit 1
    }
    rm -f "$raw_bad_prefix_log" "$raw_bad_prefix_out"
    if "$smoke_bin" --demuxer raw --input-res ' 16x16' --fps 25 --frames 1 --crf 30 -o "$raw_bad_prefix_out" "$raw" >"$raw_bad_prefix_log" 2>&1; then
        printf '%s\n' "accepted raw input resolution leading space: $raw_bad_prefix_out" >&2
        exit 1
    fi
    grep -q "invalid resolution" "$raw_bad_prefix_log" ||
    {
        printf '%s\n' "missing raw leading-space resolution parse error in $raw_bad_prefix_log" >&2
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
    for bad_param_scalar_prefix in \
        'ref:+1' 'ref: 1' \
        'crf:+23' 'crf: 23' \
        'fps:+25' 'fps: 25' \
        'fps:25/+1' 'fps:25/ 1'
    do
        bad_param_name=${bad_param_scalar_prefix%%:*}
        bad_param_value=${bad_param_scalar_prefix#*:}
        rm -f "$param_bad_scalar_prefix_log" "$param_bad_scalar_prefix_out"
        if "$smoke_bin" --demuxer y4m --frames 1 "--$bad_param_name" "$bad_param_value" --crf 30 -o "$param_bad_scalar_prefix_out" "$y4m" >"$param_bad_scalar_prefix_log" 2>&1; then
            printf '%s\n' "accepted prefixed scalar parameter --$bad_param_name '$bad_param_value': $param_bad_scalar_prefix_out" >&2
            exit 1
        fi
        grep -q "invalid argument: $bad_param_name" "$param_bad_scalar_prefix_log" ||
        {
            printf '%s\n' "missing prefixed scalar parameter parse error for --$bad_param_name '$bad_param_value' in $param_bad_scalar_prefix_log" >&2
            exit 1
        }
    done
    rm -f "$cli_bad_int_prefix_log" "$cli_bad_int_prefix_out"
    if "$smoke_bin" --demuxer y4m --frames +1 --crf 30 -o "$cli_bad_int_prefix_out" "$y4m" >"$cli_bad_int_prefix_log" 2>&1; then
        printf '%s\n' "accepted CLI integer signed prefix: $cli_bad_int_prefix_out" >&2
        exit 1
    fi
    grep -q "invalid argument: frames" "$cli_bad_int_prefix_log" ||
    {
        printf '%s\n' "missing CLI signed-prefix integer parse error in $cli_bad_int_prefix_log" >&2
        exit 1
    }
    rm -f "$cli_bad_int_prefix_log" "$cli_bad_int_prefix_out"
    if "$smoke_bin" --demuxer y4m --frames ' 1' --crf 30 -o "$cli_bad_int_prefix_out" "$y4m" >"$cli_bad_int_prefix_log" 2>&1; then
        printf '%s\n' "accepted CLI integer leading space: $cli_bad_int_prefix_out" >&2
        exit 1
    fi
    grep -q "invalid argument: frames" "$cli_bad_int_prefix_log" ||
    {
        printf '%s\n' "missing CLI leading-space integer parse error in $cli_bad_int_prefix_log" >&2
        exit 1
    }
    rm -f "$cli_bad_uint_prefix_log" "$cli_bad_uint_prefix_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --priming +1 --crf 30 -o "$cli_bad_uint_prefix_out" "$y4m" >"$cli_bad_uint_prefix_log" 2>&1; then
        printf '%s\n' "accepted CLI unsigned integer signed prefix: $cli_bad_uint_prefix_out" >&2
        exit 1
    fi
    grep -q "invalid argument: priming" "$cli_bad_uint_prefix_log" ||
    {
        printf '%s\n' "missing CLI signed-prefix unsigned integer parse error in $cli_bad_uint_prefix_log" >&2
        exit 1
    }
    rm -f "$cli_bad_uint_prefix_log" "$cli_bad_uint_prefix_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --priming ' 1' --crf 30 -o "$cli_bad_uint_prefix_out" "$y4m" >"$cli_bad_uint_prefix_log" 2>&1; then
        printf '%s\n' "accepted CLI unsigned integer leading space: $cli_bad_uint_prefix_out" >&2
        exit 1
    fi
    grep -q "invalid argument: priming" "$cli_bad_uint_prefix_log" ||
    {
        printf '%s\n' "missing CLI leading-space unsigned integer parse error in $cli_bad_uint_prefix_log" >&2
        exit 1
    }
    rm -f "$cli_bad_display_prefix_log" "$cli_bad_display_prefix_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --force-display-size +16x16 --crf 30 -o "$cli_bad_display_prefix_out" "$y4m" >"$cli_bad_display_prefix_log" 2>&1; then
        printf '%s\n' "accepted CLI display size signed prefix: $cli_bad_display_prefix_out" >&2
        exit 1
    fi
    grep -q "invalid syntax for specifying display size" "$cli_bad_display_prefix_log" ||
    {
        printf '%s\n' "missing CLI signed-prefix display size parse error in $cli_bad_display_prefix_log" >&2
        exit 1
    }
    rm -f "$cli_bad_display_prefix_log" "$cli_bad_display_prefix_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --force-display-size ' 16x16' --crf 30 -o "$cli_bad_display_prefix_out" "$y4m" >"$cli_bad_display_prefix_log" 2>&1; then
        printf '%s\n' "accepted CLI display size leading space: $cli_bad_display_prefix_out" >&2
        exit 1
    fi
    grep -q "invalid syntax for specifying display size" "$cli_bad_display_prefix_log" ||
    {
        printf '%s\n' "missing CLI leading-space display size parse error in $cli_bad_display_prefix_log" >&2
        exit 1
    }
    rm -f "$cli_bad_display_prefix_log" "$cli_bad_display_prefix_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --force-display-size 16x+16 --crf 30 -o "$cli_bad_display_prefix_out" "$y4m" >"$cli_bad_display_prefix_log" 2>&1; then
        printf '%s\n' "accepted CLI display height signed prefix: $cli_bad_display_prefix_out" >&2
        exit 1
    fi
    grep -q "invalid syntax for specifying display size" "$cli_bad_display_prefix_log" ||
    {
        printf '%s\n' "missing CLI signed-prefix display height parse error in $cli_bad_display_prefix_log" >&2
        exit 1
    }
    rm -f "$cli_bad_display_prefix_log" "$cli_bad_display_prefix_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --force-display-size '16x 16' --crf 30 -o "$cli_bad_display_prefix_out" "$y4m" >"$cli_bad_display_prefix_log" 2>&1; then
        printf '%s\n' "accepted CLI display height leading space: $cli_bad_display_prefix_out" >&2
        exit 1
    fi
    grep -q "invalid syntax for specifying display size" "$cli_bad_display_prefix_log" ||
    {
        printf '%s\n' "missing CLI leading-space display height parse error in $cli_bad_display_prefix_log" >&2
        exit 1
    }
    for bad_float_opt in abitrate aquality acodec-quality; do
        for bad_float_value in '+1.0' ' 1.0'; do
            rm -f "$cli_bad_float_prefix_log" "$cli_bad_float_prefix_out"
            if "$smoke_bin" --demuxer y4m --frames 1 "--$bad_float_opt" "$bad_float_value" --crf 30 -o "$cli_bad_float_prefix_out" "$y4m" >"$cli_bad_float_prefix_log" 2>&1; then
                printf '%s\n' "accepted CLI float prefix for --$bad_float_opt '$bad_float_value': $cli_bad_float_prefix_out" >&2
                exit 1
            fi
            grep -q "invalid argument: $bad_float_opt" "$cli_bad_float_prefix_log" ||
            {
                printf '%s\n' "missing CLI float prefix parse error for --$bad_float_opt '$bad_float_value' in $cli_bad_float_prefix_log" >&2
                exit 1
            }
        done
    done
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
    "$smoke_bin" --demuxer y4m --frames 1 --sar '1: 1' --crf 30 -o "$param_list_space_out" "$y4m" >/dev/null
    [ -s "$param_list_space_out" ] || { printf '%s\n' "missing sar separator-space smoke output: $param_list_space_out (input: $y4m)" >&2; exit 1; }
    rm -f "$param_list_space_out"
    "$smoke_bin" --demuxer y4m --frames 1 --deblock '1: 1' --crf 30 -o "$param_list_space_out" "$y4m" >/dev/null
    [ -s "$param_list_space_out" ] || { printf '%s\n' "missing deblock separator-space smoke output: $param_list_space_out (input: $y4m)" >&2; exit 1; }
    rm -f "$param_list_space_out"
    "$smoke_bin" --demuxer y4m --frames 1 --aq3-ifactor '1.1: 1.2' --crf 30 -o "$param_list_space_out" "$y4m" >/dev/null
    [ -s "$param_list_space_out" ] || { printf '%s\n' "missing aq3-ifactor separator-space smoke output: $param_list_space_out (input: $y4m)" >&2; exit 1; }
    rm -f "$param_list_space_out"
    for bad_param_list_case in \
        'sar|+1:1|signed-prefix first sar value' \
        'sar| 1:1|leading-space first sar value' \
        'sar|1:+1|signed-prefix second sar value' \
        'sar|1: +1|signed-prefix second sar value after separator space' \
        'deblock|+1:1|signed-prefix first deblock value' \
        'deblock| 1:1|leading-space first deblock value' \
        'deblock|1:+1|signed-prefix second deblock value' \
        'deblock|1: +1|signed-prefix second deblock value after separator space' \
        'aq3-ifactor|+1.1:1.2|signed-prefix first aq3-ifactor value' \
        'aq3-ifactor| 1.1:1.2|leading-space first aq3-ifactor value' \
        'aq3-ifactor|1.1:+1.2|signed-prefix second aq3-ifactor value' \
        'aq3-ifactor|1.1: +1.2|signed-prefix second aq3-ifactor value after separator space'
    do
        bad_param_name=${bad_param_list_case%%|*}
        bad_param_rest=${bad_param_list_case#*|}
        bad_param_value=${bad_param_rest%%|*}
        bad_param_message=${bad_param_rest#*|}
        rm -f "$param_bad_list_prefix_log" "$param_bad_list_prefix_out"
        if "$smoke_bin" --demuxer y4m --frames 1 "--$bad_param_name" "$bad_param_value" --crf 30 -o "$param_bad_list_prefix_out" "$y4m" >"$param_bad_list_prefix_log" 2>&1; then
            printf '%s\n' "accepted $bad_param_message: $param_bad_list_prefix_out" >&2
            exit 1
        fi
        grep -q "invalid argument: $bad_param_name" "$param_bad_list_prefix_log" ||
        {
            printf '%s\n' "missing parameter list prefix parse error for $bad_param_message in $param_bad_list_prefix_log" >&2
            exit 1
        }
    done
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
    for bad_mastering_display in \
        'G(+1,2)B(3,4)R(5,6)WP(7,8)L(9,10)' \
        'G( 1,2)B(3,4)R(5,6)WP(7,8)L(9,10)' \
        'G(1,+2)B(3,4)R(5,6)WP(7,8)L(9,10)' \
        'G(1,2)B(+3,4)R(5,6)WP(7,8)L(9,10)' \
        'G(1,2)B(3,4)R(5,6)WP(7,8)L(+9,10)' \
        'G(1,2)B(3,4)R(5,6)WP(7,8)L(9,+10)'
    do
        rm -f "$param_bad_mastering_log" "$param_bad_mastering_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --mastering-display "$bad_mastering_display" --crf 30 -o "$param_bad_mastering_out" "$y4m" >"$param_bad_mastering_log" 2>&1; then
            printf '%s\n' "accepted prefixed mastering-display '$bad_mastering_display': $param_bad_mastering_out" >&2
            exit 1
        fi
        grep -q "invalid argument: mastering-display" "$param_bad_mastering_log" ||
        {
            printf '%s\n' "missing prefixed mastering-display parse error for '$bad_mastering_display' in $param_bad_mastering_log" >&2
            exit 1
        }
    done
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
    "$smoke_bin" --demuxer y4m --frames 1 --cqm4 "1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1" --crf 30 -o "$param_cqm4_space_out" "$y4m" >/dev/null
    [ -s "$param_cqm4_space_out" ] || { printf '%s\n' "missing cqm4 separator-space smoke output: $param_cqm4_space_out (input: $y4m)" >&2; exit 1; }
    for bad_cqm4_value in \
        "+1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1" \
        " 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1" \
        "1,+1,1,1,1,1,1,1,1,1,1,1,1,1,1,1" \
        "1, +1,1,1,1,1,1,1,1,1,1,1,1,1,1,1"
    do
        rm -f "$param_bad_cqm4_log" "$param_bad_cqm4_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --cqm4 "$bad_cqm4_value" --crf 30 -o "$param_bad_cqm4_out" "$y4m" >"$param_bad_cqm4_log" 2>&1; then
            printf '%s\n' "accepted prefixed cqm4 coefficient '$bad_cqm4_value': $param_bad_cqm4_out" >&2
            exit 1
        fi
        grep -q "invalid argument: cqm4" "$param_bad_cqm4_log" ||
        {
            printf '%s\n' "missing prefixed cqm4 parse error for '$bad_cqm4_value' in $param_bad_cqm4_log" >&2
            exit 1
        }
    done
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
    for bad_pad in '+0,0,0,0,0,0,0,0,0' ' 0,0,0,0,0,0,0,0,0' '0,0,0,0,0,0,+0,0,0' '0,0,0,0,0,0, 0,0,0'; do
        rm -f "$filter_bad_pad_log" "$filter_bad_pad_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --vf "pad:$bad_pad" --crf 30 -o "$filter_bad_pad_out" "$y4m" >"$filter_bad_pad_log" 2>&1; then
            printf '%s\n' "accepted prefixed pad filter value '$bad_pad': $filter_bad_pad_out" >&2
            exit 1
        fi
        grep -q "pad .* is invalid" "$filter_bad_pad_log" ||
        {
            printf '%s\n' "missing prefixed pad filter parse error for '$bad_pad' in $filter_bad_pad_log" >&2
            exit 1
        }
    done
    rm -f "$filter_bad_crop_log" "$filter_bad_crop_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --vf crop:0x,0,0,0 --crf 30 -o "$filter_bad_crop_out" "$y4m" >"$filter_bad_crop_log" 2>&1; then
        printf '%s\n' "accepted malformed crop filter value: $filter_bad_crop_out" >&2
        exit 1
    fi
    grep -q "left crop value.*is invalid" "$filter_bad_crop_log" ||
    {
        printf '%s\n' "missing crop filter parse error in $filter_bad_crop_log" >&2
        exit 1
    }
    for bad_crop in '+0,0,0,0' ' 0,0,0,0'; do
        rm -f "$filter_bad_crop_log" "$filter_bad_crop_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --vf "crop:$bad_crop" --crf 30 -o "$filter_bad_crop_out" "$y4m" >"$filter_bad_crop_log" 2>&1; then
            printf '%s\n' "accepted prefixed crop filter value '$bad_crop': $filter_bad_crop_out" >&2
            exit 1
        fi
        grep -q "left crop value.*is invalid" "$filter_bad_crop_log" ||
        {
            printf '%s\n' "missing prefixed crop filter parse error for '$bad_crop' in $filter_bad_crop_log" >&2
            exit 1
        }
    done
    rm -f "$filter_bad_depth_log" "$filter_bad_depth_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --vf depth_8:8x --crf 30 -o "$filter_bad_depth_out" "$y4m" >"$filter_bad_depth_log" 2>&1; then
        printf '%s\n' "accepted malformed depth filter value: $filter_bad_depth_out" >&2
        exit 1
    fi
    grep -q "unsupported bit depth conversion" "$filter_bad_depth_log" ||
    {
        printf '%s\n' "missing depth filter parse error in $filter_bad_depth_log" >&2
        exit 1
    }
    for bad_depth in '+8' ' 8'; do
        rm -f "$filter_bad_depth_log" "$filter_bad_depth_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --vf "depth_8:$bad_depth" --crf 30 -o "$filter_bad_depth_out" "$y4m" >"$filter_bad_depth_log" 2>&1; then
            printf '%s\n' "accepted prefixed depth filter bit depth '$bad_depth': $filter_bad_depth_out" >&2
            exit 1
        fi
        grep -q "unsupported bit depth conversion" "$filter_bad_depth_log" ||
        {
            printf '%s\n' "missing prefixed depth filter parse error for '$bad_depth' in $filter_bad_depth_log" >&2
            exit 1
        }
    done
    "$smoke_bin" --demuxer y4m --frames 1 --vf hqdn3d:.5,3.0,6,0 --crf 30 -o "$filter_hqdn3d_out" "$y4m" >/dev/null
    [ -s "$filter_hqdn3d_out" ] || { printf '%s\n' "missing hqdn3d filter smoke output: $filter_hqdn3d_out (input: $y4m)" >&2; exit 1; }
    for bad_hqdn3d in '+4' ' 4' '4,+3' '4, 3' '4,3,+6' '4,3, 6' '4,3,6,+0' '4,3,6, 0' '-1'; do
        rm -f "$filter_bad_hqdn3d_log" "$filter_bad_hqdn3d_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --vf "hqdn3d:$bad_hqdn3d" --crf 30 -o "$filter_bad_hqdn3d_out" "$y4m" >"$filter_bad_hqdn3d_log" 2>&1; then
            printf '%s\n' "accepted malformed hqdn3d filter strength '$bad_hqdn3d': $filter_bad_hqdn3d_out" >&2
            exit 1
        fi
        grep -q "invalid options" "$filter_bad_hqdn3d_log" ||
        {
            printf '%s\n' "missing hqdn3d filter parse error for '$bad_hqdn3d' in $filter_bad_hqdn3d_log" >&2
            exit 1
        }
    done
    rm -f "$filter_bad_select_every_log" "$filter_bad_select_every_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --vf select_every:2x,0 --crf 30 -o "$filter_bad_select_every_out" "$y4m" >"$filter_bad_select_every_log" 2>&1; then
        printf '%s\n' "accepted malformed select_every filter step: $filter_bad_select_every_out" >&2
        exit 1
    fi
    grep -q "invalid step" "$filter_bad_select_every_log" ||
    {
        printf '%s\n' "missing select_every filter parse error in $filter_bad_select_every_log" >&2
        exit 1
    }
    for bad_select_every in '+2,0' ' 2,0' '2,+0' '2, 0'; do
        rm -f "$filter_bad_select_every_log" "$filter_bad_select_every_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --vf "select_every:$bad_select_every" --crf 30 -o "$filter_bad_select_every_out" "$y4m" >"$filter_bad_select_every_log" 2>&1; then
            printf '%s\n' "accepted prefixed select_every filter value '$bad_select_every': $filter_bad_select_every_out" >&2
            exit 1
        fi
        grep -Eq "invalid (step|offset)" "$filter_bad_select_every_log" ||
        {
            printf '%s\n' "missing prefixed select_every filter parse error for '$bad_select_every' in $filter_bad_select_every_log" >&2
            exit 1
        }
    done
    rm -f "$filter_empty_select_every_log" "$filter_empty_select_every_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --vf select_every:2,,0 --crf 30 -o "$filter_empty_select_every_out" "$y4m" >"$filter_empty_select_every_log" 2>&1; then
        printf '%s\n' "accepted empty select_every filter offset: $filter_empty_select_every_out" >&2
        exit 1
    fi
    grep -q "empty offset" "$filter_empty_select_every_log" ||
    {
        printf '%s\n' "missing empty select_every filter parse error in $filter_empty_select_every_log" >&2
        exit 1
    }
    rm -f "$filter_bad_yadif_log" "$filter_bad_yadif_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --vf yadif:2x --crf 30 -o "$filter_bad_yadif_out" "$y4m" >"$filter_bad_yadif_log" 2>&1; then
        printf '%s\n' "accepted malformed yadif filter mode: $filter_bad_yadif_out" >&2
        exit 1
    fi
    grep -q "invalid mode" "$filter_bad_yadif_log" ||
    {
        printf '%s\n' "missing yadif filter parse error in $filter_bad_yadif_log" >&2
        exit 1
    }
    for bad_yadif_mode in '+2' ' 2'; do
        rm -f "$filter_bad_yadif_log" "$filter_bad_yadif_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --vf "yadif:$bad_yadif_mode" --crf 30 -o "$filter_bad_yadif_out" "$y4m" >"$filter_bad_yadif_log" 2>&1; then
            printf '%s\n' "accepted prefixed yadif filter mode '$bad_yadif_mode': $filter_bad_yadif_out" >&2
            exit 1
        fi
        grep -q "invalid mode" "$filter_bad_yadif_log" ||
        {
            printf '%s\n' "missing prefixed yadif filter parse error for '$bad_yadif_mode' in $filter_bad_yadif_log" >&2
            exit 1
        }
    done
    if grep -q '^#define HAVE_SWSCALE 1$' "$smoke_dir/config.h"; then
        rm -f "$filter_bad_resize_width_log" "$filter_bad_resize_width_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --vf resize:width=16x,height=16 --crf 30 -o "$filter_bad_resize_width_out" "$y4m" >"$filter_bad_resize_width_log" 2>&1; then
            printf '%s\n' "accepted malformed resize width: $filter_bad_resize_width_out" >&2
            exit 1
        fi
        grep -q "invalid width" "$filter_bad_resize_width_log" ||
        {
            printf '%s\n' "missing resize width parse error in $filter_bad_resize_width_log" >&2
            exit 1
        }
        for bad_resize_size in 'width=+16,height=16' 'width= 16,height=16' 'width=16,height=+16' 'width=16,height= 16'; do
            rm -f "$filter_bad_resize_width_log" "$filter_bad_resize_width_out"
            if "$smoke_bin" --demuxer y4m --frames 1 --vf "resize:$bad_resize_size" --crf 30 -o "$filter_bad_resize_width_out" "$y4m" >"$filter_bad_resize_width_log" 2>&1; then
                printf '%s\n' "accepted prefixed resize dimension '$bad_resize_size': $filter_bad_resize_width_out" >&2
                exit 1
            fi
            grep -Eq "invalid (width|height)" "$filter_bad_resize_width_log" ||
            {
                printf '%s\n' "missing prefixed resize dimension parse error for '$bad_resize_size' in $filter_bad_resize_width_log" >&2
                exit 1
            }
        done
        rm -f "$filter_bad_resize_depth_log" "$filter_bad_resize_depth_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --vf resize:csp=i420:8x --crf 30 -o "$filter_bad_resize_depth_out" "$y4m" >"$filter_bad_resize_depth_log" 2>&1; then
            printf '%s\n' "accepted malformed resize csp bit depth: $filter_bad_resize_depth_out" >&2
            exit 1
        fi
        grep -q "invalid bit depth" "$filter_bad_resize_depth_log" ||
        {
            printf '%s\n' "missing resize bit-depth parse error in $filter_bad_resize_depth_log" >&2
            exit 1
        }
        for bad_resize_sar in 'sar=+1:1' 'sar=1:+1' 'sar=1: 1' 'sar=1/+1'; do
            rm -f "$filter_bad_resize_sar_log" "$filter_bad_resize_sar_out"
            if "$smoke_bin" --demuxer y4m --frames 1 --vf "resize:$bad_resize_sar" --crf 30 -o "$filter_bad_resize_sar_out" "$y4m" >"$filter_bad_resize_sar_log" 2>&1; then
                printf '%s\n' "accepted malformed resize SAR '$bad_resize_sar': $filter_bad_resize_sar_out" >&2
                exit 1
            fi
            grep -q "invalid sar" "$filter_bad_resize_sar_log" ||
            {
                printf '%s\n' "missing resize SAR parse error for '$bad_resize_sar' in $filter_bad_resize_sar_log" >&2
                exit 1
            }
        done
    fi
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
    printf '%s\n' "INTRA4X4_LUMA = 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1" > "$cqmfile_short"
    rm -f "$cqmfile_short_log" "$cqmfile_short_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --cqmfile "$cqmfile_short" --crf 30 -o "$cqmfile_short_out" "$y4m" >"$cqmfile_short_log" 2>&1; then
        printf '%s\n' "accepted short CQM file list: $cqmfile_short" >&2
        exit 1
    fi
    grep -q "not enough coefficients in list 'INTRA4X4_LUMA'" "$cqmfile_short_log" ||
    {
        printf '%s\n' "missing short CQM file list parse error in $cqmfile_short_log" >&2
        exit 1
    }
    printf '%s\n' "INTRA4X4_LUMA = 1,,1,1,1,1,1,1,1,1,1,1,1,1,1,1" > "$cqmfile_empty"
    rm -f "$cqmfile_empty_log" "$cqmfile_empty_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --cqmfile "$cqmfile_empty" --crf 30 -o "$cqmfile_empty_out" "$y4m" >"$cqmfile_empty_log" 2>&1; then
        printf '%s\n' "accepted empty CQM file coefficient: $cqmfile_empty" >&2
        exit 1
    fi
    grep -q "not enough coefficients in list 'INTRA4X4_LUMA'" "$cqmfile_empty_log" ||
    {
        printf '%s\n' "missing empty CQM file coefficient parse error in $cqmfile_empty_log" >&2
        exit 1
    }
    printf '%s\n' "INTRA4X4_LUMA = 256,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1" > "$cqmfile_large"
    rm -f "$cqmfile_large_log" "$cqmfile_large_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --cqmfile "$cqmfile_large" --crf 30 -o "$cqmfile_large_out" "$y4m" >"$cqmfile_large_log" 2>&1; then
        printf '%s\n' "accepted large CQM file coefficient: $cqmfile_large" >&2
        exit 1
    fi
    grep -q "bad coefficient in list 'INTRA4X4_LUMA'" "$cqmfile_large_log" ||
    {
        printf '%s\n' "missing large CQM file coefficient parse error in $cqmfile_large_log" >&2
        exit 1
    }
    for bad_cqmfile_prefix in '+1' '+0'; do
        printf '%s\n' "INTRA4X4_LUMA = $bad_cqmfile_prefix,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1" > "$cqmfile_prefixed"
        rm -f "$cqmfile_prefixed_log" "$cqmfile_prefixed_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --cqmfile "$cqmfile_prefixed" --crf 30 -o "$cqmfile_prefixed_out" "$y4m" >"$cqmfile_prefixed_log" 2>&1; then
            printf '%s\n' "accepted prefixed CQM file coefficient '$bad_cqmfile_prefix': $cqmfile_prefixed" >&2
            exit 1
        fi
        grep -q "bad coefficient in list 'INTRA4X4_LUMA'" "$cqmfile_prefixed_log" ||
        {
            printf '%s\n' "missing prefixed CQM file coefficient parse error for '$bad_cqmfile_prefix' in $cqmfile_prefixed_log" >&2
            exit 1
        }
    done
    for bad_tc_header_text in '# timecode format v2junk' '# timecode format v+1'; do
        printf '%s\n' "$bad_tc_header_text" > "$tc_bad_header"
        rm -f "$tc_bad_header_log" "$tc_bad_header_out"
        if "$smoke_bin" --demuxer y4m --tcfile-in "$tc_bad_header" --frames 1 --crf 30 -o "$tc_bad_header_out" "$y4m" >"$tc_bad_header_log" 2>&1; then
            printf '%s\n' "accepted malformed timecode header '$bad_tc_header_text': $tc_bad_header" >&2
            exit 1
        fi
        grep -q "unsupported timecode format" "$tc_bad_header_log" ||
        {
            printf '%s\n' "missing malformed timecode header parse error for '$bad_tc_header_text' in $tc_bad_header_log" >&2
            exit 1
        }
    done
    for bad_tc_tdecimate_frame in '-1' '+1'; do
        {
            printf '%s\n' '# timecode format v1'
            printf '%s\n' 'assume 25'
            printf '# TDecimate Mode 3: Last Frame = %s\n' "$bad_tc_tdecimate_frame"
        } > "$tc_bad_tdecimate"
        rm -f "$tc_bad_tdecimate_log" "$tc_bad_tdecimate_out"
        if "$smoke_bin" --demuxer y4m --tcfile-in "$tc_bad_tdecimate" --frames 1 --crf 30 -o "$tc_bad_tdecimate_out" "$y4m" >"$tc_bad_tdecimate_log" 2>&1; then
            printf '%s\n' "accepted malformed TDecimate last-frame count '$bad_tc_tdecimate_frame': $tc_bad_tdecimate" >&2
            exit 1
        fi
        grep -q "invalid tcfile frame count" "$tc_bad_tdecimate_log" ||
        {
            printf '%s\n' "missing malformed TDecimate parse error for '$bad_tc_tdecimate_frame' in $tc_bad_tdecimate_log" >&2
            exit 1
        }
    done
    {
        printf '%s\n' '# timecode format v1'
        printf '%s\n' 'assume 25'
        printf '%s\n' '# TDecimate Mode 3: Last Frame = 0'
    } > "$tc_tdecimate"
    rm -f "$tc_tdecimate_out"
    "$smoke_bin" --demuxer y4m --tcfile-in "$tc_tdecimate" --frames 1 --crf 30 -o "$tc_tdecimate_out" "$y4m" >/dev/null
    [ -s "$tc_tdecimate_out" ] || { printf '%s\n' "missing TDecimate tcfile smoke output: $tc_tdecimate_out (input: $tc_tdecimate)" >&2; exit 1; }
    {
        printf '%s\n' '# timecode format v1'
        printf '%s\n' 'assume 25'
        printf '%s\n' '0, 1, 30'
    } > "$tc_bad_double"
    rm -f "$tc_bad_double_out"
    "$smoke_bin" --demuxer y4m --tcfile-in "$tc_bad_double" --frames 1 --crf 30 -o "$tc_bad_double_out" "$y4m" >/dev/null
    [ -s "$tc_bad_double_out" ] || { printf '%s\n' "missing tcfile separator-space smoke output: $tc_bad_double_out (input: $tc_bad_double)" >&2; exit 1; }
    for bad_tc_double_case in \
        'v1-assume-plus|# timecode format v1
assume +25
0,1,30|tcfile parsing error: assumed fps not found' \
        'v1-range-start-plus|# timecode format v1
assume 25
+0,1,30|invalid input tcfile' \
        'v1-range-start-space|# timecode format v1
assume 25
 0,1,30|invalid input tcfile' \
        'v1-range-end-plus|# timecode format v1
assume 25
0,+1,30|invalid input tcfile' \
        'v1-range-plus|# timecode format v1
assume 25
0,1,+30|invalid input tcfile' \
        'v2-first-plus|# timecode format v2
+0
40|invalid input tcfile for frame 0' \
        'v2-first-space|# timecode format v2
 0
40|invalid input tcfile for frame 0' \
        'v2-next-plus|# timecode format v2
0
+40|invalid input tcfile for frame 1' \
        'v2-next-space|# timecode format v2
0
 40|invalid input tcfile for frame 1'
    do
        bad_tc_double_name=${bad_tc_double_case%%|*}
        bad_tc_double_rest=${bad_tc_double_case#*|}
        bad_tc_double_text=${bad_tc_double_rest%|*}
        bad_tc_double_error=${bad_tc_double_rest##*|}
        printf '%s\n' "$bad_tc_double_text" > "$tc_bad_double"
        rm -f "$tc_bad_double_log" "$tc_bad_double_out"
        if "$smoke_bin" --demuxer y4m --tcfile-in "$tc_bad_double" --frames 1 --crf 30 -o "$tc_bad_double_out" "$y4m" >"$tc_bad_double_log" 2>&1; then
            printf '%s\n' "accepted malformed tcfile value $bad_tc_double_name: $tc_bad_double" >&2
            exit 1
        fi
        grep -q "$bad_tc_double_error" "$tc_bad_double_log" ||
        {
            printf '%s\n' "missing malformed tcfile value parse error for $bad_tc_double_name in $tc_bad_double_log" >&2
            exit 1
        }
    done
    {
        printf '%s\n' '# timecode format v2'
        printf '%s\n' '0'
    } > "$tc_timebase"
    for bad_timebase in '+1/1000' ' 1/1000' '1/+1000' '1/ 1000'; do
        rm -f "$tc_bad_timebase_log" "$tc_bad_timebase_out"
        if "$smoke_bin" --demuxer y4m --tcfile-in "$tc_timebase" --timebase "$bad_timebase" --frames 1 --crf 30 -o "$tc_bad_timebase_out" "$y4m" >"$tc_bad_timebase_log" 2>&1; then
            printf '%s\n' "accepted prefixed tcfile timebase '$bad_timebase': $tc_bad_timebase_out" >&2
            exit 1
        fi
        grep -q "invalid argument: timebase" "$tc_bad_timebase_log" ||
        {
            printf '%s\n' "missing tcfile timebase parse error for '$bad_timebase' in $tc_bad_timebase_log" >&2
            exit 1
        }
    done
    rm -f "$stats_y4m" "$stats_valid" "$stats_pass1_out" \
          "$stats_bad_resolution" "$stats_bad_resolution_out" "$stats_bad_resolution_log" \
          "$stats_bad_timebase" "$stats_bad_timebase_out" "$stats_bad_timebase_log" \
          "$stats_bad_main" "$stats_bad_main_out" "$stats_bad_main_log" \
          "$stats_bad_main_ref" "$stats_bad_main_ref_out" "$stats_bad_main_ref_log" \
          "$stats_bad_ref" "$stats_bad_ref_out" "$stats_bad_ref_log" \
          "$stats_bad_delim" "$stats_bad_delim_out" "$stats_bad_delim_log" \
          "$stats_weight" "$stats_weight_out" "$stats_weight_spaces" "$stats_weight_spaces_out" \
          "$stats_weight_chroma_spaces" "$stats_weight_chroma_spaces_out" \
          "$stats_bad_weight" "$stats_bad_weight_out" "$stats_bad_weight_log" \
          "$stats_bad_bframes" "$stats_bad_bframes_out" "$stats_bad_bframes_log" \
          "$stats_bad_bframes_ws" "$stats_bad_bframes_ws_out" "$stats_bad_bframes_ws_log" \
          "$stats_bad_lookahead_ws" "$stats_bad_lookahead_ws_out" "$stats_bad_lookahead_ws_log"
    write_smoke_y4m_frames "$stats_y4m" 2 25:1
    "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 1 --no-mbtree --bframes 0 --ref 1 --stats "$stats_valid" -o "$stats_pass1_out" "$stats_y4m" >/dev/null
    [ -s "$stats_valid" ] || { printf '%s\n' "missing two-pass stats smoke output: $stats_valid" >&2; exit 1; }
    for bad_stats_resolution in '+16x16' '16x+16' '16x 16'; do
        sed "1s|#options: [^ ]*|#options: $bad_stats_resolution|" "$stats_valid" > "$stats_bad_resolution"
        rm -f "$stats_bad_resolution_log" "$stats_bad_resolution_out"
        if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_resolution" -o "$stats_bad_resolution_out" "$stats_y4m" >"$stats_bad_resolution_log" 2>&1; then
            printf '%s\n' "accepted malformed stats resolution '$bad_stats_resolution': $stats_bad_resolution" >&2
            exit 1
        fi
        grep -q "resolution specified in stats file not valid" "$stats_bad_resolution_log" ||
        {
            printf '%s\n' "missing malformed stats resolution parse error for '$bad_stats_resolution' in $stats_bad_resolution_log" >&2
            exit 1
        }
    done
    for bad_stats_timebase in '-1/1' '+1/1' '1/+1' '1/ 1'; do
        sed "1s|timebase=[^ ]*|timebase=$bad_stats_timebase|" "$stats_valid" > "$stats_bad_timebase"
        rm -f "$stats_bad_timebase_log" "$stats_bad_timebase_out"
        if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_timebase" -o "$stats_bad_timebase_out" "$stats_y4m" >"$stats_bad_timebase_log" 2>&1; then
            printf '%s\n' "accepted malformed stats timebase '$bad_stats_timebase': $stats_bad_timebase" >&2
            exit 1
        fi
        grep -q "timebase specified in stats file not valid" "$stats_bad_timebase_log" ||
        {
            printf '%s\n' "missing malformed stats timebase parse error for '$bad_stats_timebase' in $stats_bad_timebase_log" >&2
            exit 1
        }
    done
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
    sed '2s|q:\([^ ]*\)|q:+\1|' "$stats_valid" > "$stats_bad_main"
    rm -f "$stats_bad_main_log" "$stats_bad_main_out"
    if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_main" -o "$stats_bad_main_out" "$stats_y4m" >"$stats_bad_main_log" 2>&1; then
        printf '%s\n' "accepted prefixed stats main field: $stats_bad_main" >&2
        exit 1
    fi
    grep -q "statistics are damaged at line 0" "$stats_bad_main_log" ||
    {
        printf '%s\n' "missing prefixed stats main-field parse error in $stats_bad_main_log" >&2
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
    sed '3s|ref:0 ;|ref:+0 ;|' "$stats_valid" > "$stats_bad_ref"
    rm -f "$stats_bad_ref_log" "$stats_bad_ref_out"
    if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_ref" -o "$stats_bad_ref_out" "$stats_y4m" >"$stats_bad_ref_log" 2>&1; then
        printf '%s\n' "accepted prefixed stats ref list: $stats_bad_ref" >&2
        exit 1
    fi
    grep -q "statistics are damaged at line 1" "$stats_bad_ref_log" ||
    {
        printf '%s\n' "missing prefixed stats ref-list parse error in $stats_bad_ref_log" >&2
        exit 1
    }
    sed '3s|ref:0 ;|ref:0 w:0,1,0 ;|' "$stats_valid" > "$stats_weight"
    "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_weight" -o "$stats_weight_out" "$stats_y4m" >/dev/null
    [ -s "$stats_weight_out" ] || { printf '%s\n' "missing stats weight smoke output: $stats_weight_out" >&2; exit 1; }
    sed '3s|;||' "$stats_weight" > "$stats_bad_delim"
    if "$smoke_bin" --demuxer y4m --frames 1 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_delim" -o "$stats_bad_delim_out" "$stats_y4m" >"$stats_bad_delim_log" 2>&1; then
        printf '%s\n' "accepted unterminated stats record: $stats_bad_delim" >&2
        exit 1
    fi
    grep -q "statistics are damaged at line 1" "$stats_bad_delim_log" ||
    {
        printf '%s\n' "missing unterminated stats-record parse error in $stats_bad_delim_log" >&2
        exit 1
    }
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
    sed '3s|ref:0 ;|ref:0 w:+0,1,0 ;|' "$stats_valid" > "$stats_bad_weight"
    rm -f "$stats_bad_weight_log" "$stats_bad_weight_out"
    if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_weight" -o "$stats_bad_weight_out" "$stats_y4m" >"$stats_bad_weight_log" 2>&1; then
        printf '%s\n' "accepted prefixed stats weight list: $stats_bad_weight" >&2
        exit 1
    fi
    grep -q "statistics are damaged at line 1" "$stats_bad_weight_log" ||
    {
        printf '%s\n' "missing prefixed stats weight-list parse error in $stats_bad_weight_log" >&2
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
    sed '1s|bframes=[^ ]*|bframes=+0|' "$stats_valid" > "$stats_bad_bframes"
    rm -f "$stats_bad_bframes_log" "$stats_bad_bframes_out"
    if "$smoke_bin" --demuxer y4m --frames 2 --bitrate 100 --pass 2 --no-mbtree --bframes 0 --ref 1 --stats "$stats_bad_bframes" -o "$stats_bad_bframes_out" "$stats_y4m" >"$stats_bad_bframes_log" 2>&1; then
        printf '%s\n' "accepted prefixed stats bframes token: $stats_bad_bframes" >&2
        exit 1
    fi
    grep -q "bframes specified in stats file not valid" "$stats_bad_bframes_log" ||
    {
        printf '%s\n' "missing prefixed stats bframes parse error in $stats_bad_bframes_log" >&2
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
    write_smoke_y4m_with_header "$y4m_tail_fps" 1 "YUV4MPEG2 W16 H16 Ip A1:1 C420 F25:1"
    "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$y4m_tail_fps_out" "$y4m_tail_fps" >/dev/null
    [ -s "$y4m_tail_fps_out" ] || { printf '%s\n' "missing Y4M tail frame-rate smoke output: $y4m_tail_fps_out (input: $y4m_tail_fps)" >&2; exit 1; }
    if grep -q '^#define HAVE_SWSCALE 1$' "$smoke_dir/config.h"; then
        write_smoke_y4m_with_header "$y4m_mono" 1 "YUV4MPEG2 W16 H16 F25:1 Ip A1:1 Cmono"
        "$smoke_bin" --demuxer y4m --input-csp i400 --frames 1 --crf 30 -o "$y4m_mono_out" "$y4m_mono" >/dev/null
        [ -s "$y4m_mono_out" ] || { printf '%s\n' "missing Y4M mono tail colorspace smoke output: $y4m_mono_out (input: $y4m_mono)" >&2; exit 1; }
    fi
    write_smoke_y4m_with_header "$y4m_color_range_tail" 1 "YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420 XCOLORRANGE=FULL"
    "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$y4m_color_range_tail_out" "$y4m_color_range_tail" >/dev/null
    [ -s "$y4m_color_range_tail_out" ] || { printf '%s\n' "missing Y4M tail color-range smoke output: $y4m_color_range_tail_out (input: $y4m_color_range_tail)" >&2; exit 1; }
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
    for bad_zone in '+0,0,b=1.0' '0,+0,b=1.0' '0,0,q=+20' '0,0,q= 20' '0,0,b=+1.0' '0,0,b= 1.0'; do
        rm -f "$zone_prefixed_log" "$zone_prefixed_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --zones "$bad_zone" --crf 30 -o "$zone_prefixed_out" "$y4m" >"$zone_prefixed_log" 2>&1; then
            printf '%s\n' "accepted zone prefixed value '$bad_zone': $zone_prefixed_out" >&2
            exit 1
        fi
        grep -q "invalid zone" "$zone_prefixed_log" ||
        {
            printf '%s\n' "missing zone prefixed value parse error for '$bad_zone' in $zone_prefixed_log" >&2
            exit 1
        }
    done
    rm -f "$zone_empty_opt_log" "$zone_empty_opt_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --zones "0,0,b=1,,subme=1" --crf 30 -o "$zone_empty_opt_out" "$y4m" >"$zone_empty_opt_log" 2>&1; then
        printf '%s\n' "accepted zone with empty option token: $zone_empty_opt_out" >&2
        exit 1
    fi
    grep -q "empty zone param" "$zone_empty_opt_log" ||
    {
        printf '%s\n' "missing empty zone param parse error in $zone_empty_opt_log" >&2
        exit 1
    }
    rm -f "$device_empty_log" "$device_empty_out"
    if "$smoke_bin" --demuxer y4m --frames 1 --device "bluray,,psp" --crf 30 -o "$device_empty_out" "$y4m" >"$device_empty_log" 2>&1; then
        printf '%s\n' "accepted device with empty token: $device_empty_out" >&2
        exit 1
    fi
    grep -q "empty device" "$device_empty_log" ||
    {
        printf '%s\n' "missing empty device parse error in $device_empty_log" >&2
        exit 1
    }
    printf '%s\n' '0 I none' > "$qp_none"
    "$smoke_bin" --quiet --demuxer y4m --frames 1 --qpfile "$qp_none" -o "$qp_none_out" "$y4m" >/dev/null
    [ -s "$qp_none_out" ] || { printf '%s\n' "missing qpfile none smoke output: $qp_none_out (input: $y4m)" >&2; exit 1; }
    for bad_qpfile_prefix in '+0 I 20' '0 I +20'; do
        printf '%s\n' "$bad_qpfile_prefix" > "$qp_trailing"
        rm -f "$qp_trailing_log" "$qp_trailing_out"
        if "$smoke_bin" --demuxer y4m --frames 1 --qpfile "$qp_trailing" -o "$qp_trailing_out" "$y4m" >"$qp_trailing_log" 2>&1; then
            :
        fi
        grep -q "can't parse qpfile for frame 0" "$qp_trailing_log" ||
        {
            printf '%s\n' "missing qpfile prefixed-integer parse error for '$bad_qpfile_prefix' in $qp_trailing_log" >&2
            exit 1
        }
    done
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
    isolate_source_root_configs_for_smoke
    (
        cd "$build_dir"
        PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-$msys2_pkg_config_path} \
        CC=${CC:-cc} RC= "$root/configure" --enable-static --disable-cli \
            --disable-audio --disable-avs --disable-lavf --disable-ffms --disable-lsmash --chroma-format=420 --bit-depth="$bit_depth"
        make -j"$make_jobs" checkasm
        if [ -x ./checkasm${bit_depth}.exe ]; then
            ./checkasm${bit_depth}.exe 0
            bad_seed_log=$build_dir/checkasm-bad-seed.log
            for bad_seed in 0x +1 ' 1' -1; do
                if ./checkasm${bit_depth}.exe "$bad_seed" >"$bad_seed_log" 2>&1; then
                    printf '%s\n' "accepted malformed checkasm seed '$bad_seed': $bad_seed_log" >&2
                    exit 1
                fi
                grep -q "invalid random seed" "$bad_seed_log" ||
                {
                    printf '%s\n' "missing checkasm seed parse error for '$bad_seed' in $bad_seed_log" >&2
                    exit 1
                }
            done
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
