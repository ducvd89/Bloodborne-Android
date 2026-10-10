# Sourced by capture_all.sh and verify.sh: the Proton builds a capture may use, the vkd3d-proton
# settings, and fsr4cap_run, which starts fsr4cap.exe under a Proton build (PROTONPATH).
#
# proton_candidates lists them best first; capture_all.sh keeps the first one under which the
# DLL starts FSR 4.1. Measured on an RX 7800 XT (2026-10-07): GE-Proton 11-6, Proton-CachyOS 11
# (July) and Valve's Proton - Experimental (October) start it; Valve's Proton 11.0-2c and
# GE-Proton 9 do not (their vkd3d-proton offers only the DLL's FSR 3/2 providers, issue #4).
# PROTONPATH set: that one only.
# The DLL picks its shader variant from what vkd3d-proton offers: with FP8 cooperative matrices
# (RDNA4) it dispatches another variant than the one vk_fsr411.cpp replays (issue #12). Hidden
# here; on an RX 7800 XT the capture is the same byte for byte either way.
export VKD3D_DISABLE_EXTENSIONS=${VKD3D_DISABLE_EXTENSIONS-VK_KHR_cooperative_matrix,VK_NV_cooperative_matrix2,VK_EXT_shader_float8}
export WINEDEBUG=${WINEDEBUG:--all}

# Steam installs (native, ~/.steam, Flatpak) and their library folders, one per line.
steam_roots() {
    local d
    for d in "$HOME/.local/share/Steam" "$HOME/.steam/root" "$HOME/.steam/steam" \
             "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"; do
        if [[ -d $d/steamapps ]]; then realpath "$d"; fi
    done | awk '!seen[$0]++'
}
steam_libraries() {
    local root
    while IFS= read -r root; do
        echo "$root"
        sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' \
            "$root/steamapps/libraryfolders.vdf" 2>/dev/null
    done < <(steam_roots) | awk '!seen[$0]++'
}
# The folder of Steam tool <appid> (the container runtime a Proton build asks for).
steam_tool_dir() {
    local lib dir
    while IFS= read -r lib; do
        dir=$(sed -n 's/^[[:space:]]*"installdir"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' \
              "$lib/steamapps/appmanifest_$1.acf" 2>/dev/null)
        if [[ -n $dir && -d $lib/steamapps/common/$dir ]]; then
            echo "$lib/steamapps/common/$dir"
            return 0
        fi
    done < <(steam_libraries)
    return 1
}

# Proton builds to try, best first, one folder per line: GE-Proton 10 or newer (newest first),
# Proton-CachyOS (Steam's or a distribution package under /usr/share/steam), Valve's Proton -
# Experimental, Valve's Proton 11.0 and 10.0. Each once, also when reached through a symlink
# ("Proton-GE Latest").
proton_candidates() {
    if [[ -n ${PROTONPATH:-} ]]; then
        echo "$PROTONPATH"
        return
    fi
    local root lib p
    local tools=()
    while IFS= read -r root; do
        tools+=("$root/compatibilitytools.d")
    done < <(steam_roots)
    tools+=("$HOME/.steam/root/compatibilitytools.d" "$HOME/.local/share/Steam/compatibilitytools.d")
    # Where Steam also looks: distribution packages (Arch/CachyOS proton-cachyos, Fedora, ...)
    # install system-wide, and STEAM_EXTRA_COMPAT_TOOLS_PATHS (colon-separated) adds folders.
    local extra
    IFS=: read -ra extra <<< "${STEAM_EXTRA_COMPAT_TOOLS_PATHS:-}"
    tools+=("${extra[@]}" /usr/local/share/steam/compatibilitytools.d
            /usr/share/steam/compatibilitytools.d)
    {
        # GE-Proton: version order (`ls | tail -1` took GE-Proton9-27 over GE-Proton10-x).
        for root in "${tools[@]}"; do
            for p in "$root"/GE-Proton*; do
                if [[ -f $p/proton && ${p##*/} =~ ^GE-Proton([0-9]+)- && ${BASH_REMATCH[1]} -ge 10 ]]; then
                    printf '%s\t%s\n' "${p##*/}" "$p"
                fi
            done
        done | sort -V -r -k1,1 | cut -f2
        # Proton-CachyOS: the native Steam's before Flatpak's, newest first within each.
        for root in "${tools[@]}"; do
            for p in "$root"/*; do
                if [[ -f $p/proton && ${p,,} == *cachyos* ]]; then
                    printf '%s\t%s\n' "${p##*/}" "$p"
                fi
            done | sort -V -r -k1,1 | cut -f2
        done
        while IFS= read -r lib; do
            for p in "$lib/steamapps/common/Proton - Experimental" "$lib/steamapps/common/Proton 11.0" \
                     "$lib/steamapps/common/Proton 10.0"; do
                if [[ -f $p/proton ]]; then echo "$p"; fi
            done
        done < <(steam_libraries)
    } | while IFS= read -r p; do
        # The same folder by device and inode: realpath kept /home/... and /var/home/... apart
        # where /home is a bind mount (Fedora Atomic, Bazzite; issue #61).
        printf '%s\t%s\n' "$(stat -L -c %d:%i -- "$p" 2>/dev/null || realpath -- "$p")" "$p"
    done | awk -F'\t' '!seen[$1]++ { print $2 }'
}

# Whether <log> shows that the container (pressure-vessel, also under umu-run) could not start:
# no unprivileged user namespaces, typically when the launcher itself runs in a sandbox (Flatpak,
# a Steam shortcut with a runtime) or the kernel forbids them (issue #61).
container_failed() {
    grep -qE 'bwrap: (No permissions to create a new namespace|Creating new namespace failed|setting up uid map)|pressure-vessel-wrap\[[0-9]+\]: E: ' "$1" 2>/dev/null
}

# Why a run failed, in one line: the recorder's last words, else the container's or Wine's error,
# else the last line of the Proton log.
failure_reason() {
    local R=$1 line
    line=$(grep -v '^[[:space:]]*$' "$R/fsr4cap.log" 2>/dev/null | tail -1)
    [[ -n $line ]] || line=$(grep -E 'bwrap:|E: |err:|[Ee]rror' "$R/umu.log" 2>/dev/null | tail -1)
    [[ -n $line ]] || line=$(grep -v '^[[:space:]]*$' "$R/umu.log" 2>/dev/null | tail -1)
    echo "${line:-no output (log: $R/umu.log)}"
}

# fsr4cap_run <dir holding fsr4cap.exe and the DLLs> <fsr4cap.exe arguments...> (in a subshell:
# it changes directory): output in <dir>/umu.log. With umu-run installed it runs there (prefix
# <dir>/pfx-<Proton>); otherwise
# (the AppImage: umu is not bundled) Proton runs in the Steam container runtime it asks for
# (toolmanifest.vdf: require_tool_appid; Steam installs it with the first game run with this
# Proton), prefix <dir>/compat-<Proton>/pfx. BB_FSR4CAP_RUNNER=steam: the latter even with umu-run.
# When the container cannot start (no user namespaces, see container_failed), Proton runs directly
# on the host, without a container runtime, from then on (marker <dir>/.direct);
# BB_FSR4CAP_RUNNER=direct: that from the start.
fsr4cap_run() (
    local R=$1
    shift
    # The container sees the work folder (under $HOME), not necessarily the current one (the
    # AppImage's, or a private /tmp): "bwrap: Can't chdir".
    cd -- "$R" || return
    # A prefix per Proton build: one prefix used by several versions is upgraded and downgraded.
    local name=${PROTONPATH##*/}
    name=${name// /_}
    local direct=0 status=0
    if [[ ${BB_FSR4CAP_RUNNER:-} == direct || -e $R/.direct ]]; then
        direct=1
    fi
    if [[ $direct == 0 && ${BB_FSR4CAP_RUNNER:-} != steam ]] && command -v umu-run >/dev/null; then
        WINEPREFIX=$R/pfx-$name GAMEID=umu-fsr4cap umu-run "$R/fsr4cap.exe" "$@" > "$R/umu.log" 2>&1 ||
            status=$?
        container_failed "$R/umu.log" || return "$status"
        fsr4cap_container_fallback "$R"
        direct=1
    fi
    local appid dir root tools=$PROTONPATH
    local runtime=()
    appid=$(sed -n 's/.*"require_tool_appid"[[:space:]]*"\([0-9]*\)".*/\1/p' \
            "$PROTONPATH/toolmanifest.vdf" 2>/dev/null)
    if [[ $direct == 1 ]]; then
        appid=
        tools=$PROTONPATH
    fi
    if [[ -n $appid ]]; then
        if ! dir=$(steam_tool_dir "$appid"); then
            echo "Steam's container runtime $appid, which $PROTONPATH needs, is not installed:" \
                 "start any game with this Proton in Steam once (Steam installs it), or install" \
                 "umu-launcher." >&2
            exit 3
        fi
        runtime=("$dir/_v2-entry-point" --verb=waitforexitandrun --)
        tools=$PROTONPATH:$dir
    fi
    root=$(steam_roots | head -1)
    mkdir -p "$R/compat-$name"
    run_proton() {
        STEAM_COMPAT_DATA_PATH=$R/compat-$name STEAM_COMPAT_CLIENT_INSTALL_PATH=$root \
            STEAM_COMPAT_INSTALL_PATH=$R STEAM_COMPAT_TOOL_PATHS=$tools STEAM_COMPAT_APP_ID=0 \
            SteamAppId=0 "$@" "$PROTONPATH/proton" waitforexitandrun "$R/fsr4cap.exe" "${args[@]}" \
            > "$R/umu.log" 2>&1
    }
    local args=("$@")
    status=0
    run_proton "${runtime[@]}" || status=$?
    if [[ ${#runtime[@]} -gt 0 ]] && container_failed "$R/umu.log"; then
        fsr4cap_container_fallback "$R"
        tools=$PROTONPATH
        status=0
        run_proton || status=$?
    fi
    return "$status"
)

# The container could not start: say so once and run Proton directly from then on.
fsr4cap_container_fallback() {
    if [[ ! -e $1/.direct ]]; then
        echo "  Steam's container could not start here ($(grep -m1 -oE 'bwrap: [^.]*' "$1/umu.log" ||
              echo 'pressure-vessel failed')): running Proton without it" >&2
        : > "$1/.direct"
    fi
}
