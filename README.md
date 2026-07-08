# Net Stat Changes Geode Mod

This is a small Geode mod source package for Geometry Dash 2.2081.

## Build

1. Install Geode and Geode CLI.
2. Put this folder somewhere convenient.
3. Run:

```bash
geode build
```

The built `.geode` package should appear in the build folder.

## Usage

Open Geometry Dash, then click the **star** button on the main menu.

The mod auto-saves a snapshot at most once every 12 hours when you reach the main menu. You can also click **Snapshot Now** in the popup.

## Notes

- History starts when the mod is installed.
- Demon keys are tracked through the internal stat commonly known as `BASEMENT_KEYS`.
- The snapshot file is saved in the mod's Geode save directory as `snapshots.tsv`.


## v4 note
Updated Popup usage for Geode v5: `Popup` is no longer templated and `initAnchored` was replaced by `Popup::init`.
