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
make_jobs=${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '%s\n' 1)}
CONFIG_ARGS=${CONFIG_ARGS:---disable-cli}
msys2_root=${MSYS2_ROOT:-/d/msys64}
msys2_clang64=${MSYS2_CLANG64:-$msys2_root/clang64}
msys2_usr_local=${MSYS2_USR_LOCAL:-$msys2_root/usr/local}
msys2_root_diag=$(cygpath -m "$msys2_root" 2>/dev/null || printf '%s\n' "$msys2_root")
msys2_clang64_diag=$(cygpath -m "$msys2_clang64" 2>/dev/null || printf '%s\n' "$msys2_clang64")
msys2_usr_local_diag=$(cygpath -m "$msys2_usr_local" 2>/dev/null || printf '%s\n' "$msys2_usr_local")
msys2_pkg_config_path=${MSYS2_PKG_CONFIG_PATH:-$msys2_usr_local/lib/pkgconfig:$msys2_usr_local/share/pkgconfig:$msys2_clang64/lib/pkgconfig:$msys2_clang64/share/pkgconfig}

export TMP=${TMP:-$tmpdir}
export TEMP=${TEMP:-$tmpdir}
export TMPDIR=${TMPDIR:-$tmpdir}
export PATH=${MSYS2_PATH:-$msys2_clang64/bin:$msys2_root/usr/bin}:$PATH

mkdir -p "$build_root" "$TMPDIR"

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
    y4m=$1
    {
        printf '%s\n' 'YUV4MPEG2 W16 H16 F25:1 Ip A1:1 C420'
        printf '%s\n' 'FRAME'
        dd if=/dev/zero bs=384 count=1 2>/dev/null
    } > "$y4m"
}

run_smoke_cli()
{
    configure_smoke_cli smoke-cli "--disable-lavf --disable-ffms --disable-lsmash --disable-audio"
    y4m=$smoke_dir/smoke.y4m
    out=$smoke_dir/smoke.264
    write_smoke_y4m "$y4m"
    "$smoke_bin" --demuxer y4m --frames 1 --crf 30 -o "$out" "$y4m" >/dev/null
    [ -s "$out" ] || { printf '%s\n' "missing CLI smoke output: $out (input: $y4m)" >&2; exit 1; }
    printf '%s\n' "smoke-cli output: $out ($(wc -c < "$out") bytes)"
}

run_smoke_output_mp4()
{
    command -v ffprobe >/dev/null 2>&1 || { printf '%s\n' 'ffprobe is required for smoke-output:mp4 external dependency smoke; this is not a core GNU17 failure. Install ffprobe or adjust PATH.' >&2; exit 1; }
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

run_smoke_output()
{
    run_smoke_output_mp4
    run_smoke_output_gop
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
        printf '%s\n' "usage: $0 [whitespace|builds|consumers|warnings|warnings-extra|warnings-extra:double|warnings-extra:conversion|warnings-extra:external|warnings-extra:compare|warnings-extra:baseline-save|warnings-extra:delta|feature-probe|avi-legacy-probe|smoke-cli|smoke-output|smoke-output:mp4|smoke-output:gop|shared|shared-consumer|shared-8b|shared-consumers|checkasm-smoke|checkasm-smoke-8b|checkasm-smoke-10b|all]" >&2
        exit 2
        ;;
esac
