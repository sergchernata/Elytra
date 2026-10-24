# Elytra

**Elytra is an Arc theme for the [Ladybird](https://ladybird.org) browser.** It gives Ladybird the interface of
[Arc](https://arc.net): a sidebar of spaces, pinned tabs and favorites, and the rest of Arc's way of browsing, on
top of Ladybird's own independent engine.

It's a fork that tracks upstream Ladybird closely. The Arc interface lives in its own directory (`UI/Qt/Arc`), and
the few changes it needs in Ladybird itself are small, marked `Fork hook (Arc)`, and listed in
[SYNCING.md](SYNCING.md), so it stays easy to bring in Ladybird's latest work.

> [!NOTE]
> Elytra isn't affiliated with The Browser Company (the makers of Arc) or with the Ladybird project. Like Ladybird, it's
> pre-alpha software.

## What's in it

- **Spaces**: each with its own color, icon, pinned tabs and folders, and optionally its own profile. Switch with a
  swipe, the switcher at the bottom of the sidebar, or ⌘⌥← / ⌘⌥→.
- **Pinned tabs and favorites** that remember their page, with "Back to Pinned URL" when you wander off.
- **Today's tabs**, which tidy themselves away, as in Arc.
- **A sidebar that hides**: toggle it with ⌘S or the button beside the window controls, and it slides out when you
  point at the window's edge.
- **The command bar** (⌘T, ⌘L) for searching, opening and switching tabs.
- **Little Arc** (⌘⌥N, from any app): a small window for a quick look, which you can move into a space.
- **Picture-in-Picture as in Arc**: leave a playing video's tab and it floats over everything, with Back to Tab,
  Minimize and Close. Minimized, it plays on with a player at the bottom of the sidebar.
- **Fill from 1Password** (⌘\), through 1Password's command-line tool.
- **Import from Arc**: your spaces, pinned tabs, folders and favorites.
- **Updates itself** from this repository's signed releases.

## Install

Download the latest build for Apple Silicon Macs from [Releases](../../releases). It's signed ad hoc, not notarized,
so the first time, right-click the app and choose Open (or run `xattr -dr com.apple.quarantine` on it).

## Build

Elytra builds just like Ladybird; see [the build instructions](Documentation/BuildInstructionsLadybird.md). The Arc
interface is part of the Qt UI, which Ladybird uses on desktop platforms.

---

# Ladybird

[Ladybird](https://ladybird.org) is a truly independent web browser, using a novel engine based on web standards.

> [!IMPORTANT]
> Ladybird is in a pre-alpha state, and only suitable for use by developers
>

## Features

We aim to build a complete, usable browser for the modern web.

Ladybird uses a multi-process architecture with a main UI process, several WebContent renderer processes,
an ImageDecoder process, and a RequestServer process.

Image decoding and network connections are done out of process to be more robust against malicious content.
Each tab has its own renderer process, which is sandboxed from the rest of the system.

At the moment, many core library support components are inherited from SerenityOS:

- LibWeb: Web rendering engine
- LibJS: JavaScript engine
- LibWasm: WebAssembly implementation
- LibCrypto/LibTLS: Cryptography primitives and Transport Layer Security
- LibHTTP: HTTP/1.1 client
- LibGfx: 2D Graphics Library, Image Decoding and Rendering
- LibUnicode: Unicode and locale support
- LibMedia: Audio and video playback
- LibCore: Event loop, OS abstraction layer
- LibIPC: Inter-process communication

## How do I build and run this?

See [build instructions](Documentation/BuildInstructionsLadybird.md) for information on how to build Ladybird.

Ladybird runs on Linux, macOS, Windows (with WSL2), and many other \*Nixes.

## How do I read the documentation?

Code-related documentation can be found in the [documentation](Documentation/) folder.

## Get in touch and participate!

Join [our Discord server](https://discord.gg/nvfjVJ4Svh) to participate in issue and development discussions.

Please read [Getting involved with Ladybird](Documentation/GettingStartedContributing.md) if you're new to Ladybird and want to help.

Before opening an issue, please see the [issue policy](CONTRIBUTING.md#issue-policy) and the [detailed issue-reporting guidelines](ISSUES.md).

The project participation guidelines can be found in [`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

Ladybird is licensed under a 2-clause BSD license.
