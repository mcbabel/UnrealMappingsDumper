#!/usr/bin/env bash
set -euo pipefail

usage() {
	cat <<USAGE
Usage: $(basename "$0") [options] [process name]

Injects UnrealMappingsDumper.dll into a Windows game running under Proton or Wine.

  process name       Name (or part of it) of the game executable, e.g. MyGame-Win64-Shipping.exe.
                     Without it the only running *-Win64-Shipping.exe process is used.
  --pid <pid>        Linux pid of the game process.
  --dll <path>       Dll to inject, defaults to UnrealMappingsDumper.dll next to this script.
  --injector <path>  Injector to use, defaults to UnrealMappingsInjector.exe next to this script.
  -h, --help         Show this help.

The mappings and UnrealMappingsDumper.log are written next to the game executable.
USAGE
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DLL="$SCRIPT_DIR/UnrealMappingsDumper.dll"
INJECTOR="$SCRIPT_DIR/UnrealMappingsInjector.exe"
PATTERN=""
GAME_PID=""

while [[ $# -gt 0 ]]; do
	case "$1" in
		-h|--help) usage; exit 0 ;;
		--pid) GAME_PID="${2:?--pid needs a value}"; shift 2 ;;
		--dll) DLL="${2:?--dll needs a value}"; shift 2 ;;
		--injector) INJECTOR="${2:?--injector needs a value}"; shift 2 ;;
		-*) echo "Unknown option $1" >&2; usage >&2; exit 1 ;;
		*) PATTERN="$1"; shift ;;
	esac
done

for file in "$DLL" "$INJECTOR"; do
	if [[ ! -f "$file" ]]; then
		echo "$file doesn't exist." >&2
		exit 1
	fi
done

DLL="$(realpath "$DLL")"
INJECTOR="$(realpath "$INJECTOR")"

is_wine_process() {
	local exe
	exe="$(readlink "/proc/$1/exe" 2>/dev/null || true)"
	[[ "$(basename "$exe")" == wine* ]]
}

windows_exe_name() {
	local first
	first="$(tr '\0' '\n' < "/proc/$1/cmdline" 2>/dev/null | head -n 1)"
	first="${first##*\\}"
	first="${first##*/}"
	printf '%s' "$first"
}

if [[ -z "$GAME_PID" ]]; then
	matches=()

	for dir in /proc/[0-9]*; do
		pid="${dir#/proc/}"
		is_wine_process "$pid" || continue

		name="$(windows_exe_name "$pid")"
		[[ "${name,,}" == *.exe ]] || continue

		if [[ -n "$PATTERN" ]]; then
			[[ "${name,,}" == *"${PATTERN,,}"* ]] || continue
		else
			[[ "${name,,}" == *-win64-* || "${name,,}" == *-wingdk-* ]] || continue
		fi

		matches+=("$pid")
	done

	if [[ ${#matches[@]} -eq 0 ]]; then
		echo "No matching game process found. Start the game first, or pass its process name or --pid." >&2
		exit 1
	fi

	if [[ ${#matches[@]} -gt 1 ]]; then
		echo "More than one process matches, pass the exact name or --pid:" >&2
		for pid in "${matches[@]}"; do
			printf '%8s  %s\n' "$pid" "$(windows_exe_name "$pid")" >&2
		done
		exit 1
	fi

	GAME_PID="${matches[0]}"
fi

if [[ ! -r "/proc/$GAME_PID/environ" ]]; then
	echo "Can't read the environment of process $GAME_PID." >&2
	exit 1
fi

if ! is_wine_process "$GAME_PID"; then
	echo "Process $GAME_PID isn't running under Wine/Proton." >&2
	exit 1
fi

GAME_EXE="$(windows_exe_name "$GAME_PID")"
WINE_DIR="$(dirname "$(readlink "/proc/$GAME_PID/exe")")"

RUN=()
if [[ "$(readlink /proc/self/ns/mnt)" != "$(readlink "/proc/$GAME_PID/ns/mnt")" ]]; then
	if ! command -v nsenter > /dev/null; then
		echo "The game runs in a container (Steam Linux Runtime), nsenter from util-linux is needed to reach it." >&2
		exit 1
	fi

	RUN=(nsenter --target "$GAME_PID" --mount --root)
	if [[ "$(readlink /proc/self/ns/user)" != "$(readlink "/proc/$GAME_PID/ns/user")" ]]; then
		RUN=(nsenter --target "$GAME_PID" --user --preserve-credentials --mount --root)
	fi
	RUN+=(--)
fi

in_game_ns() {
	${RUN[@]+"${RUN[@]}"} "$@"
}

for file in "$DLL" "$INJECTOR"; do
	if ! in_game_ns test -f "$file"; then
		echo "$file isn't visible inside the game's container. Put the files somewhere inside your home directory." >&2
		exit 1
	fi
done

WINE=""
for candidate in "$WINE_DIR/wine64" "$WINE_DIR/wine" "$(command -v wine64 || true)" "$(command -v wine || true)"; do
	if [[ -n "$candidate" ]] && in_game_ns test -x "$candidate"; then
		WINE="$candidate"
		break
	fi
done

if [[ -z "$WINE" ]]; then
	echo "Could not find the wine binary the game is running with (looked in $WINE_DIR)." >&2
	exit 1
fi

GAME_ENV=()
while IFS= read -r -d '' entry; do
	case "$entry" in
		LD_PRELOAD=*|WINESERVERSOCKET=*|WINEPRELOADRESERVE=*|WINELOADERNOEXEC=*) ;;
		*) GAME_ENV+=("$entry") ;;
	esac
done < "/proc/$GAME_PID/environ"

WINDOWS_DLL="Z:${DLL//\//\\}"

echo "Game:      $GAME_EXE (pid $GAME_PID)"
echo "Wine:      $WINE"
echo "Prefix:    $(printf '%s\n' "${GAME_ENV[@]}" | sed -n 's/^WINEPREFIX=//p' | head -n 1)"
echo "Container: $([[ ${#RUN[@]} -gt 0 ]] && echo yes || echo no)"
echo "Dll:       $DLL"

in_game_ns env -i "${GAME_ENV[@]}" "$WINE" "$INJECTOR" --dll "$WINDOWS_DLL" "$GAME_EXE"
