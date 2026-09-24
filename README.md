# Rift Barebones

***Rift Barebones is an unofficial, independently developed Cemu derivative that introduces a controller-friendly Virtual Portal of Power and local `.sky` file management. At least, that is the main job.***

**This started as a personal project, and it is also my first public project, so please keep that in mind when looking through the code.**

Iteration 006 is the first and current main version of Rift Barebones. (after hundreds of mini iterations...)

## Current functionality

**Virtual and supported physical Portal of Power hot swapping!!!!**

Rift adds an in-game collection drawer with controller navigation,  haptics, local `.sky` browsing, sorting, filtering, favorites, creation, placement, removal, backups, and recoverable deletion (Doesn't seem that important, but just trust me it is).
Deleting a selected local figure moves its file into the collection's `.rift-trash` folder. Rift refuses to delete files outside the configured collection and never loads `.rift-trash` or `Backups` as part of the playable catalog 
Controller setup is saved automatically. Named profiles are optional copies, while the active Player configuration won't just magically disappear and ordinary controller reconnects. Rift can also recover when Windows gives the same SDL controller a different identity after switching between USB and Bluetooth or after a driver change.
Besides that you kinda have to know the struggle of using the native Cemu virtual portal to know the benefits of Rift. 
## Screenshots and GIF

***I'll add these eventually.***

## Quick-start tutorial
(Assuming you have your games/Cemu set up correctly before hand.)
1. Download the newest Windows ZIP from the [Releases page](https://github.com/Swanikins/Rift-Barebones-Releases/releases).
2. Extract the entire ZIP into its own folder. Do not run the executable from inside the ZIP.
3. Run `Rift-Barebones.exe`.
4. Open **Options > General settings**. Add the folder containing your legally dumped Wii U games, then choose the included `skylanders` folder or your existing `.sky` collection folder.
5. Open **Options > Input settings**, choose the Wii U controller type and physical input device for Player 1, and map the required buttons. The active setup saves automatically.
6. Open **Options > Graphic packs**, click **Download latest community graphic packs**, select your Skylanders game, and enable the graphic packs you want to use. (Big thank you to FrankyBuster for updating and improving these two game's graphic packs)
7. Start your Skylanders game.
8. Open **Settings** inside Rift and select either Virtual Portal or Physical Portal. You can also adjust a handful of things here.
9. Press **F7** to open the in-game Rift of Power. With a controller, press **R + D-pad Down** to open or close it. Press **F8** to open the original desktop manager, which i'm like 75% sure is broken and not needed. But I just want to get Rift published already.

##### [Buy me a Skylander on Ko-fi!](https://ko-fi.com/swanikin)

## Controller controls

| Input | Action |
| --- | --- |
| **R + D-pad Down** | Open or close Rift. On a Wii Remote and Nunchuk, use **Z + D-pad Down**. |
| **D-pad or left stick** | Move through cards, portal slots, element filters, tabs, and settings. |
| **A** | Select, place a figure, apply a filter, change an option, or confirm an action. |
| **B** | Go back, cancel, or close Rift. |
| **X** | Remove a figure from the portal. On a collection figure's details page, request confirmed deletion of its local `.sky` file. |
| **Y** | Open figure details. On the details page, create a backup. |
| **L** | Open collection ordering. On the details page, move to the previous tab. |
| **R** | Toggle a collection favorite. On the details page, move to the next tab. |


These are the emulated Wii U controls configured for Player 1. The physical labels will be different on an Xbox, PlayStation, Nintendo, or third-party controller depending on the mappings in **Options > Input settings**. I have not added every controller glyph yet and probably won't since most of yall know what you're doing.

Confirmed deletion moves a `.sky` file into that collection folder's `.rift-trash` directory so it can be recovered.

## Development status and road map

Iteration 006 is the main Rift Barebones version. The supported workflow is to use either the Virtual Portal or a supported Physical Portal. Simultaneous hybrid physical-and-virtual operation is still experimental and is not required for the rest of Rift to work.

I still have many features I want to implement and will put together a larger road map as those plans settle. At the end of the day, it is still one person working on the project ;-;

### Disclaimer

Physical-to-virtual switching still needs testing across more hardware configurations. In other words, do not switch back and forth extremely quickly or else you're just asking for trouble. For those who decided to play with fire, just make sure to unplug you physical portal, and then restart Rift 

## Support

### [Support Rift Barebones on Ko-fi](https://ko-fi.com/swanikin)

Rift remains freely available. If you enjoy the project, optional support is available on [Ko-fi](https://ko-fi.com/swanikin). Or if you know me, SEND ME SKYLANDERS!!!

Tips do not purchase builds, features, priority, or support.

## Building

Rift uses Cemu's existing CMake and vcpkg build system. This section assumes you already know how to configure and build a CMake project with Visual Studio 2022.

```powershell
git clone --recursive https://github.com/Swanikins/Rift-Barebones.git
cd Rift-Barebones
```

Initialize all submodules, bootstrap the bundled vcpkg checkout, configure an x64 Visual Studio build, and build the `CemuBin` target in `Release`. See [BUILD.md](BUILD.md) for the required tools, dependencies, and CMake options. The `build-rift.ps1` helper is experimental, and honestly broken, and is not required.

The repository does not include games, title keys, firmware, or figure dumps. Packaged third-party portrait and catalog assets are documented in [the third-party data and asset notices](assets/THIRD_PARTY_NOTICES.md). Users must supply their own legally obtained figure files and any additional artwork.

## Disclosure

Rift Barebones is based on [Cemu](https://github.com/cemu-project/Cemu). Cemu and modified MPL-covered files are distributed under the [Mozilla Public License 2.0](LICENSE.txt). Files under `dependencies` retain their own licenses, and interface assets retain the notices included with them. Packaged figure portraits and modified catalog data are attributed in [the third-party data and asset notices](assets/THIRD_PARTY_NOTICES.md).
AI-assisted tools were used during portions of implementation and documentation. The maintainer (me) is responsible for reviewing, testing, and publishing the resulting work. Rift is in active development. It is not affiliated with or endorsed by the Cemu project, Activision, Toys for Bob, Microsoft, or Nintendo. Skylanders and related names remain the property of their respective owners. (I need to put this here to protect my own butt from several *entitys*, It's kind of unavoidable nowadays.)
