# UnrealMappingsDumper

[![Build](https://github.com/mcbabel/UnrealMappingsDumper/actions/workflows/build.yml/badge.svg)](https://github.com/mcbabel/UnrealMappingsDumper/actions/workflows/build.yml)

Generates `.usmap` mappings files for Unreal Engine games. Mappings are needed by tools like [FModel](https://fmodel.app), [CUE4Parse](https://github.com/FabianFG/CUE4Parse), UAssetGUI/UAssetAPI and UE4SS to read the assets of games that are cooked with unversioned properties, which is the case for almost every UE5 game.

Be sure to check the [Mappings Archive](https://github.com/OutTheShade/Unreal-Mappings-Archive) if the mappings for your game have already been dumped before. If you dump mappings for a game that isn't in the archive yet, uploading them there is encouraged.

**Platforms:** Windows and Linux are both officially supported. On Linux there is a native dumper that works with native Linux games as well as with Windows games running under Steam Proton, Wine, Lutris or Bottles, without needing Wine itself.

## Features

- Works on UE 4.25 up to the latest UE5 releases, including games built with `UE_FNAME_OUTLINE_NUMBER` (e.g. Fortnite) and the newer, smaller `FField` layout of recent UE5 versions.
- No hardcoded game specific offsets needed: GObjects, the name pool and all member offsets (`UObject`, `UStruct`, `FField`, `FProperty`, `UEnum`) are detected and validated at runtime. Signatures are only a first guess, everything is double checked before it's used.
- Never takes the game down: memory is only read after checking it's readable, and if something unexpected still happens, the error is caught and written to the log instead of crashing the game.
- Writes the current usmap format (version 4, `ExplicitEnumValues`): long names, enums with more than 255 entries and enums with non sequential values are stored correctly. Also supports `OptionalProperty`, `Utf8StrProperty`, `AnsiStrProperty` and game specific property classes.
- Everything is logged to `UnrealMappingsDumper.log`, which makes it easy to report problems.
- Native Linux version that reads the game from outside: no injection, no Wine needed, works for native Linux games and Proton/Wine games alike.
- Comes with an injector for Windows (also usable through Wine/Proton), no third party tools needed.

## Download

Download the latest release from [Releases](https://github.com/mcbabel/UnrealMappingsDumper/releases):

| Package | Contents |
| --- | --- |
| `UnrealMappingsDumper-*-windows.zip` | `UnrealMappingsDumper.dll`, `UnrealMappingsInjector.exe`, `inject-linux.sh` |
| `UnrealMappingsDumper-*-linux-x86_64.tar.gz` | `unreal-mappings-dumper` (native Linux), plus the Windows files for injecting into Proton/Wine games |

Builds of every commit are also available as artifacts of the [Build workflow](https://github.com/mcbabel/UnrealMappingsDumper/actions/workflows/build.yml).

**UnrealMappingsDumper is not responsible for any bans. Don't use it on games with anti cheat, or at least take the proper precautions.**

## Usage

Start the game and wait until you're in the main menu, so all classes are loaded. Then load the dll into the game.

### Windows

Put the extracted files into one folder and run in a terminal:

```
UnrealMappingsInjector.exe
```

The injector finds the running `*-Win64-Shipping.exe` process by itself. If more than one game is running, pass its name (`UnrealMappingsInjector.exe MyGame`) or its pid (`--pid 1234`). `--list` shows all running processes. Any other dll injector (System Informer, Cheat Engine, Extreme Injector, ...) works too.

Make sure you inject into the actual game process, `<Project>\Binaries\Win64\<Project>-Win64-Shipping.exe`, not into the small launcher exe in the game folder that only starts it.

### Linux (native)

`unreal-mappings-dumper` is a native Linux program. It reads the memory of the running game from outside, so nothing is injected and Wine isn't needed. It works with:

- native Linux builds of Unreal games (`<Project>-Linux-Shipping`)
- Windows games running under Steam Proton, Wine, Lutris, Bottles, ... (`<Project>-Win64-Shipping.exe`)

```bash
tar -xzf UnrealMappingsDumper-*-linux-x86_64.tar.gz
sudo ./unreal-mappings-dumper
```

Reading the memory of another process needs root. If you don't want to use sudo every time, allow it once:

```bash
sudo setcap cap_sys_ptrace=eip ./unreal-mappings-dumper
./unreal-mappings-dumper
```

The dumper finds the running `*-Win64-Shipping.exe` or `*-Linux-Shipping` process by itself. If more than one game is running, pass a part of its name (`./unreal-mappings-dumper MyGame`) or its pid (`--pid 12345`). `--list` shows all processes that can be dumped, `--output <file>` changes where the mappings are written to. When run with sudo, the written files are given back to your user.

### Linux (injecting into Proton/Wine games)

Alternatively the Windows dll can be injected into Proton/Wine games, which doesn't need root. This is the same code path as on Windows, which can help if a game only works with it (for example games with a non standard name pool, where the dll can fall back to calling the game's own `FName::ToString`).

`inject-linux.sh` finds the game process, reuses the Wine/Proton build and the environment the game is running with (prefix, esync/fsync settings, ...) and runs the injector inside the game's prefix:

```bash
chmod +x inject-linux.sh
./inject-linux.sh
```

Like the native dumper, it takes a part of the game's executable name (`./inject-linux.sh MyGame`) or the Linux pid of the game process (`--pid 12345`). `--dll` and `--injector` take custom paths.

Notes:

- Run the script as the same user as the game. It needs `bash` and, for Proton, `nsenter` from util-linux, which is installed on every common distribution.
- Newer Proton versions run games inside the Steam Linux Runtime container, Flatpak Steam runs them inside its sandbox. The script detects that and runs the injector inside the game's container, so the exact same Wine/Proton build and libraries are used. The dll and the injector must be visible inside the container: if the script says they aren't, put them into your home directory or into the game's folder.

### Output

When the dll is done, a message box shows where the files were written to, the native Linux dumper prints it:

- `Mappings.usmap` - the mappings
- `UnrealMappingsDumper.log` - everything the dumper did

Both are written next to the game executable (`<Project>/Binaries/Win64/` or `<Project>/Binaries/Linux/`). If that folder isn't writable, the dll uses its own folder and then `%TEMP%\UnrealMappingsDumper`, the native Linux dumper uses the current directory. Under Proton the game folder is in your Steam library, e.g. `~/.local/share/Steam/steamapps/common/<Game>/<Project>/Binaries/Win64/`.

The mappings are written uncompressed, which every tool supports. If `oo2core_9_win64.dll` is loaded by the game or placed next to the dumper, Oodle compression can be enabled in `dllmain.cpp`.

## Troubleshooting

| Problem | Solution |
| --- | --- |
| "Could not find GObjects" / "Could not find FName::ToString or the name pool" | The game might be too old (UE < 4.23), use a modified engine, or encrypt its names. On Linux, try injecting the dll into the Proton version, it can call the game's own `FName::ToString`. Open an issue with the log file. |
| "Not allowed to read the memory of process" (Linux) | Run the native dumper with sudo, or allow it once with `sudo setcap cap_sys_ptrace=eip ./unreal-mappings-dumper`. |
| Nothing happens after injecting | Check `UnrealMappingsDumper.log` next to the game executable. Make sure you injected into `*-Win64-Shipping.exe` and not into the launcher. |
| "Process is a 32 bit process" / "Platform mismatch" | The game is 32 bit. Unreal Engine games that need mappings are 64 bit only, 32 bit isn't supported. |
| The usmap is very small / classes are missing | Inject later, once the game has fully loaded (main menu or in game). |
| The game crashes or closes on injection | Most likely anti cheat or anti tamper protection. The dumper itself catches its own errors, check the log. |
| `LoadLibrary failed` | The dll path is wrong, or the dll was blocked by Windows (right click → Properties → Unblock). |

When reporting a problem, please attach `UnrealMappingsDumper.log`, the game name and its engine version, and a download link if the game is free.

## Building

Everything is plain C++23 without external dependencies. Only 64 bit builds are supported.

### Windows

- **Visual Studio 2022**: open `UnrealMappingsDumper.sln` and build `Release | x64`, or open the folder as a CMake project (builds the dll, the injector and the tests).
- **CMake**:

  ```
  cmake -S . -B build -A x64
  cmake --build build --config Release
  ctest --test-dir build -C Release
  ```

### Linux

The native dumper:

```bash
sudo apt install cmake g++                                  # Debian/Ubuntu, g++ 13 or newer
sudo dnf install cmake gcc-c++                              # Fedora
sudo pacman -S cmake gcc                                    # Arch

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The output is `build/unreal-mappings-dumper`.

The Windows dll and the injector can be cross compiled with MinGW-w64, the tests then run through Wine:

```bash
sudo apt install cmake g++-mingw-w64-x86-64-posix wine64    # Debian/Ubuntu
sudo dnf install cmake mingw64-gcc-c++ wine                 # Fedora
sudo pacman -S cmake mingw-w64-gcc wine                     # Arch

cmake -S . -B build-windows -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-windows
ctest --test-dir build-windows
```

### Tests

`Tests/FakeEngineTest.cpp` builds a fake engine in memory (GObjects, name pool, classes, structs, enums and properties) in several layouts that match different engine versions, runs the dumper on it and parses the resulting usmap again. On Linux the fake engine runs in a child process that is dumped from outside, like a real game. CI additionally dumps running fake games with the same tools that are used for real games: the dll injected on Windows and through Wine, and the native Linux dumper against a native and a Wine game.

## Contributing

The engine layout lives in `unrealTypes.h`, the per version defaults and signatures in `unrealVersion.h`, and the runtime detection in `unrealVersion.cpp`. All of it reads memory through `memory.h`, which has an in-process backend for the Windows dll (`Platform/Windows`) and a backend that reads another process for the native Linux dumper (`Platform/Linux`). If a game needs special handling, add a version struct in `unrealVersion.h` and select it in `InitEngine` in `app.cpp`, and please add a layout to the fake engine test if the change is about the engine layout.

Pull requests are built and tested on Windows and Linux automatically.

## Credits

Originally created by [OutTheShade](https://github.com/OutTheShade) ([TheNaeem/UnrealMappingsDumper](https://github.com/TheNaeem/UnrealMappingsDumper)). Released under the [MIT License](LICENSE).
