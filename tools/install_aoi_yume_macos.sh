#!/bin/zsh
set -euo pipefail

repo_root="${0:A:h:h}"
build_root="${BUILD_ROOT:-$repo_root/build-plugin/plugins/rompler/EONDS50_artefacts/RelWithDebInfo}"
install_root="${INSTALL_ROOT:-${HOME}/Library/Audio/Plug-Ins}"
backup_root="${BACKUP_ROOT:-$install_root/.aoi-yume-backups}"
skip_signature="${SKIP_SIGNATURE:-0}"
dry_run="${DRY_RUN:-0}"

bundle_specs=(
    "VST3|Aoi YUME.vst3"
    "AU|Aoi YUME.component"
)
canonical_fonts=("Crystal Legacy.sf2" "Natural Stage.sf2" "Studio Essentials.sf2")

require_identity() {
    local bundle="$1"
    local plist="$bundle/Contents/Info.plist"
    [[ -f "$plist" ]] || { print -u2 "Missing Info.plist: $plist"; return 1; }
    local metadata
    if plutil -lint "$plist" >/dev/null 2>&1; then
        metadata="$(plutil -p "$plist")"
        [[ "$metadata" == *'"CFBundleName" => "Aoi YUME"'* ]] || { print -u2 "Unexpected product name in $plist"; return 1; }
        [[ "$metadata" == *'"CFBundleIdentifier" => "com.eonlab.aoi-yume"'* ]] || { print -u2 "Unexpected bundle ID in $plist"; return 1; }
        if [[ "$metadata" == *'"AudioComponents"'* ]]; then
            [[ "$metadata" == *'"manufacturer" => "EonL"'* ]] || { print -u2 "Unexpected AU manufacturer in $plist"; return 1; }
            [[ "$metadata" == *'"subtype" => "AoYu"'* ]] || { print -u2 "Unexpected AU subtype in $plist"; return 1; }
        fi
    fi
}

install_subdir_for_format() {
    case "$1" in
        VST3) print VST3 ;;
        AU) print Components ;;
        *) print -u2 "Unsupported install format: $1"; return 1 ;;
    esac
}

require_canonical_fonts() {
    local bundle="$1"
    local font_dir="$bundle/Contents/Resources/SoundFonts"
    [[ -d "$font_dir" ]] || { print -u2 "Missing SoundFonts directory: $font_dir"; return 1; }
    local expected actual
    expected="$(printf '%s\n' "${canonical_fonts[@]}")"
    actual="$(find "$font_dir" -maxdepth 1 -type f -name '*.sf2' -print | sed 's|.*/||' | sort)"
    [[ "$actual" == "$expected" ]] || {
        print -u2 "Unexpected SoundFonts in $bundle:\n$actual"
        return 1
    }
    if [[ "$skip_signature" != "1" ]]; then
        codesign --verify --deep --strict "$bundle"
    fi
}

for spec in "${bundle_specs[@]}"; do
    format="${spec%%|*}"
    bundle_name="${spec#*|}"
    require_identity "$build_root/$format/$bundle_name"
    require_canonical_fonts "$build_root/$format/$bundle_name"
done

timestamp="$(date +%Y%m%d-%H%M%S)"
backup_dir="$backup_root/$timestamp"
if [[ "$dry_run" == "1" ]]; then
    print "DRY RUN: would back up existing Aoi YUME bundles to $backup_dir"
    for spec in "${bundle_specs[@]}"; do
        format="${spec%%|*}"
        bundle_name="${spec#*|}"
        install_format="$(install_subdir_for_format "$format")"
        print "DRY RUN: would install $build_root/$format/$bundle_name -> $install_root/$install_format/$bundle_name"
    done
    exit 0
fi

mkdir -p "$backup_dir"
for spec in "${bundle_specs[@]}"; do
    format="${spec%%|*}"
    bundle_name="${spec#*|}"
    install_format="$(install_subdir_for_format "$format")"
    destination="$install_root/$install_format/$bundle_name"
    source="$build_root/$format/$bundle_name"
    mkdir -p "$install_root/$install_format"
    if [[ -e "$destination" ]]; then
        mkdir -p "$backup_dir/$install_format"
        mv "$destination" "$backup_dir/$install_format/$bundle_name"
    fi
    ditto --norsrc --noqtn "$source" "$destination"
    require_identity "$destination"
    require_canonical_fonts "$destination"
done

print "Installed Aoi YUME VST3/AU bundles"
print "Backup: $backup_dir"
