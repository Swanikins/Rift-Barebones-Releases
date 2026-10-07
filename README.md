# Rift Barebones

***Rift Barebones is a controller-first version of Cemu built specifically to make Skylanders easier to set up and play.***

This started as a personal project, and it is also my first public project, so please keep that in mind when looking through the code.

The **Rift of Power** puts your collection, figure creation, portal controls, and Rift settings into one in-game menu. It is designed mainly for controllers, while mouse and keyboard remain (kinda) available. I tried my best to make the UI/UX be as intuitive as possible, borrowing many elements from the Steam OS big screen menu that I just LOVE

Iteration 006.6 supports Virtual, Physical, and Hybrid Portal modes, meaning we can use real and virtual figures together.

## What's new in Iteration 006.6?

Iteration 006.6 makes older Cemu Skylander collections work properly, including supported files organized into subfolders.

Collection messages now explain whether Rift could not find supported files or whether the current search and filters are hiding them. After an automatic update, Rift also shows a short summary of what changed.

## Why Rift?

Rift takes its name from the Rift Engines in *Skylanders SuperChargers*. The idea behind the project is that a Portal Master, desperate to revive the Skylanders universe, built the Rift of Power using the same magic that once powered the Rift Engines. (yes, IT HAS LORE!)

## Download and setup

1. Download the newest Windows ZIP from the [Releases page](https://github.com/Swanikins/Rift-Barebones-Releases/releases).
2. Extract the entire ZIP into its own folder. Do not run Rift from inside the ZIP.
3. Run `Rift-Barebones.exe`.
4. Open **Options > General settings**, add your legally dumped Wii U games, and choose the included `skylanders` folder or your existing figure collection.
5. Open **Options > Input settings** and configure Player 1. Rift saves the active controller setup automatically.
6. Open **Options > Graphic packs** and use **Download latest community graphic packs** to get the current packs from Cemu's repository.
7. Start your Skylanders game.
8. Press **F7** to open the Rift of Power. With a controller, press **R + D-pad Down**.
9. Use the portal-mode tabs at the top of the Rift of Power to hot-swap between Virtual, Physical, and Hybrid portals.

## Support

Rift is free, and open source for anyone to build off of, but if you enjoy it and want to support my Skylander addiction, you can [buy me a Skylander on Ko-fi](https://ko-fi.com/swanikin)!

## Supporters

MY FIRST SUPPORTER!!
[KeybladeMasterVicky](https://ko-fi.com/E3Z726IUN8) THANK YOU!

## Building from source

Install Git and Visual Studio 2022 with **Desktop development with C++**, **C++ CMake tools for Windows**, and a Windows 10 or 11 SDK. Then open a Developer PowerShell for Visual Studio 2022 and run:

```powershell
git clone --recursive https://github.com/Swanikins/Rift-Barebones.git
cd Rift-Barebones
.\dependencies\vcpkg\bootstrap-vcpkg.bat -disableMetrics
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 `
  -DCEMU_EXECUTABLE_NAME=Rift-Barebones
cmake --build build --target CemuBin --config Release --parallel 4
```

The finished program will be written to `bin\Rift-Barebones.exe`. The upstream platform-specific build notes remain available in [BUILD.md](BUILD.md).

The repository does not include games, title keys, firmware, or figure dumps.



## Credits and disclosure

Rift Barebones is based on [Cemu](https://github.com/cemu-project/Cemu). Cemu and Rift's modified MPL-covered files are distributed under the [Mozilla Public License 2.0](LICENSE.txt). Third-party catalog data, portraits, interface assets, metadata, and Hybrid Portal code are credited in [the third-party notices](assets/THIRD_PARTY_NOTICES.md). AI-assisted tools were used during portions of implementation and documentation. The maintainer is responsible for reviewing, testing, and publishing the resulting work. Rift is not affiliated with or endorsed by the Cemu project, Activision, Toys for Bob, Microsoft, or Nintendo. Skylanders and related names remain the property of their respective owners.
