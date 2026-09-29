# Third-party data and asset notices

This directory contains third-party material used by Rift Barebones. The notices below identify the source of that material and the technical changes made for Rift.

## EmulAnders figure portraits and catalog data

- Project: [EmulAnders](https://github.com/joaohypo/emulanders)
- Maintainer: [joaohypo](https://github.com/joaohypo)
- Distribution source: [EmulAnders releases](https://github.com/joaohypo/emulanders/releases)
- Material used by Rift: the downloadable Skylander portrait asset pack and catalog data used as the basis for `skylanders_db.json`
- Upstream repository license: GPL-3.0

Rift modifies the catalog data for its desktop collection, identification, and creation interfaces. The modified catalog uses explicit character and variant identifiers, excludes development and template records from creation menus, and maps portraits to Rift's local naming and lookup rules. Rift does not include the EmulAnders Nintendo Switch sysmodule or Tesla overlay code.

The portrait files originate from the EmulAnders asset-pack release. The EmulAnders release page does not state a separate artwork-specific license. No ownership of the underlying Skylanders characters or third-party artwork is claimed by Rift Barebones; those materials remain subject to their original rights and the terms supplied by their source.

## Skylander metadata

`skylander_metadata.json` contains normalized base-figure metadata derived from [kooscode/sky-tools](https://github.com/kooscode/sky-tools) at commit [`5d988b4db6631811fa67bf297b5550335a8c3186`](https://github.com/kooscode/sky-tools/commit/5d988b4db6631811fa67bf297b5550335a8c3186).

- Upstream author: Koos du Preez
- Source table: [`lib/skylanderDB.cpp`](https://github.com/kooscode/sky-tools/blob/5d988b4db6631811fa67bf297b5550335a8c3186/lib/skylanderDB.cpp)
- Upstream license: [GNU Lesser General Public License 2.1](https://github.com/kooscode/sky-tools/blob/5d988b4db6631811fa67bf297b5550335a8c3186/LICENSE)
- Additional sources credited upstream: [skylandersNFC](https://gist.github.com/skylandersNFC/4f0348c7e66fe9ab28e2ac3b82e549e2), [Texthead1/Skylander-IDs](https://github.com/Texthead1/Skylander-IDs), and Portal-To-Unity

Rift retains only the element and normalized UI category fields from this source. Local catalog names and portrait mappings remain authoritative. Rift corrects catalog ID 3412 from the duplicated Smash Hit entry to Spitfire, represents racing-driver IDs 3440 through 3446 separately from ordinary vehicles, and determines release generation from the variant identifier rather than the removed base-game field.

The metadata sidecar remains separately attributed under LGPL-2.1. Preserve this notice and the applicable upstream license when redistributing it.

## Format references

Rift's catalog validation and figure identification also reference:

- [Texthead1/Skylander-IDs](https://github.com/Texthead1/Skylander-IDs)
- [Runes Skylander format notes](https://github.com/NefariousTechSupport/Runes/blob/master/Docs/SkylanderFormat.md)

These references document identifiers and data layout. They do not imply endorsement of Rift Barebones.

## Hybrid Portal research and implementation

Rift's simultaneous physical-and-virtual Portal of Power support is adapted from [rhsts/cemu-hybrid-skylander-portal](https://github.com/rhsts/cemu-hybrid-skylander-portal), maintained by rhsts. Rift studied and ported the `hybrid-portal` branch through commit [`df0ff385b126e9821a355a774161bb8536588166`](https://github.com/rhsts/cemu-hybrid-skylander-portal/commit/df0ff385b126e9821a355a774161bb8536588166). The upstream work established the dedicated libusb bridge, physical figure caching, shared physical and virtual slot model, reconnect handling, write forwarding, and LED forwarding used as the basis for Rift's integration.

Those adapted source files and Rift's modifications are covered by the [Mozilla Public License 2.0](../LICENSE.txt). Rift adds its own portal-mode switching, verified physical write-back, collection UI integration, diagnostics, and safeguards around physical slots. Credit belongs to rhsts and the upstream contributors for the original hybrid portal research and implementation.
