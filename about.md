# Net Stat Changes

A read-only local snapshot logger for Geometry Dash stats. This would be insanely useful if you are an avid stargrinder and wants to see how many stars you have gained each day!

## What it does

- Adds a star button to the user profile. (Currently, the button appears on any profile menu, including other users' profiles)
- Saves local stat snapshots to this mod's Geode save folder.
- Shows your net stat change over the last 7 days (Customisable!).
- Tracks known GD 2.2 stat IDs including stars, moons, user coins, mana orbs, total orbs, diamonds, and demon keys.
- Also includes a graph feature to visualise the stat changes across a customisable period of time. The graph colors are also customisable!
- Customisable UTC timezone to set when to treat as a new day

## Important

This mod cannot reconstruct history from before you installed it. The first week will compare against your earliest available snapshot until a true snapshot exists (depending on the user settings).

It only reads stats and writes its own local snapshot file. It does not edit your GD save file or submit data anywhere.

## Note
Sorry if this mod is visually janky! This is my first time making a mod and am still exploring various features.