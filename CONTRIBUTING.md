# Contributing

> [!WARNING]
> There's **no guarantee** that every pull request submitted will be accepted.
> It's up to you to **make your case** and to make a ***convincing* argument** for why your pull request should be accepted.

## Fixing Bugs/Issues

There are three types of issues in this Repo:

- Recomp. Issues
- Game Issues
- Mod Issues

### Recomp. Issues

Relates to bugs and issues with the Recomp's repo, hooks, inputs and rendering system. Issues with *modding hooks*, *Frame Interpolation*, *Ray Tracing* and *QOL features* would be filed as a "**Recomp. Issue**". Anything that extends functionality to the game is considered a "**Recomp. Feature**".

Fixing these issues involves making changes to the Recomp. codebase and can extend to forked/external libraries like rt64 and N64ModernRuntime.

An example is Frame Interpolation which had issues with objects not being identified properly and causing objects to visually stutter as they moved.

### Game Issues

Bugs and issues that are also a part of the base game and have been around since release are filed as "**Game Issues**". These are issues that can touch hundreds of different functions and affect various internal systems in the game. Systems like *LOD*, *Object Culling*, *Camera Frustum Culling*, *Terrain Rendering*, *Texture Rendering*, *Spline Path Interpolation* and *many* ***more***.

Fixing these issues can have a knock-on effect on other systems which can affect the base experience. Instead of making changes to the Recomp. codebase to fix such issues it's preferable to instead create a mod that attempts to fix the base game's issues. If a mod requires a new hook to fix the issue then it requires **Recomp. Feature**.

An example of this is the Long Tow Cable mod which fixes an issue where older segments of the tow cable would disappear as the AT-AT legs were being wrapped. Additional features stemming from these fixes should be provided as a "**Recomp. Feature**" and/or a **Mod**.

### Mod Issues

These are issues that are external to the base game relating to how the mod itself handles game code. Sometimes it's not a matter of the game being incorrect but rather the proper hooks being used in the right way and in the right order. Correctness in this respect is really up to what the mod is intending to add/fix. If a mod requires additional functionality/hooks these new capabilities can be added as a "**Recomp. Feature**".

> [!TIP]
> **There will be some issues that overlap between these categories.** In such cases the solution is usually a combination of solutions.
> For example, 'Draw Distance' would be considered a "**Game Issue**" which in turn warrants a "**Recomp. Feature**" to allow users to adjust the draw distance from a defined base value. You'd then provide an option in the video settings file while also providing a 'modding hook'.
> A mod hook allows an external UI Mod to provide front-end functionality to the user, like a Draw Distance Slider in the Options menu.

## Making a Pull Request

1. Fork the repo, clone it with `--recursive`, and branch off `main`.
2. Build and run the game as described in [Building](README.md#building). You need your own ROM. Never commit it or anything made from it.
3. Overrides of game functions go in `rogue_squadron.toml` hooks, not hand edits to `RecompiledFuncs/`. [AGENTS.md](AGENTS.md) has the rules and the code style, and an AI agent will read it on its own.
4. Run the unit tests and style check. With a game build: `cmake --build build --config Release --target rs64_unit_tests`. Without a ROM: `cmake -S tests -B build-tests` then `cmake --build build-tests --config Release --target rs64_unit_tests`.
5. Open the PR against `main` and fill in the template, including how you tested it.

A pull request will be considered *stale* if it's open for a week or more with little to no work or word from the original contributor.

Stale pull requests will be closed after a certain amount of time.

## AI Usage

AI Agent usage **is** allowed and **encouraged** when working/contributing to this repo. This is basically a hobby project and really I don't expect to be inundated with pull requests. If you'd like to contribute feel free to make a pull request.

> [!NOTE]
> Dependent repos used in this project like *[N64Recomp](https://github.com/N64Recomp/N64Recomp)*, *[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)*, and *[rt64](https://github.com/rt64/rt64)* declared that they **DO NOT** reflect this sentiment.
> Since this project was '*made with AI*' you must **NOT** contribute anything here to those repos. They'll be unfortunately rejected outright no matter what quality/value that contribution actually brings.
> On that note, I will happily accept changes to any of the forks.

---

(This document was written by [a human](https://github.com/MikeSemicolonD).)
