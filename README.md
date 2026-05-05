# tape-delay

Warm tape echo — analog-flavored stereo delay with feedback LPF.

A PocketDAW fx plugin. Built against the [pocketdaw-sdk](https://github.com/poealone/pocketdaw-sdk).

## Build

```
make            # Linux .so
make windows    # Windows .dll (needs MinGW + SDL2 mingw dev libs)
make device     # aarch64 .so for RG35XX
make deploy     # copy artefacts into ../pocket-daw/plugins/fx/tape-delay/
```

## Manifest

See `manifest.json` for parameter list and UI metadata.

## License

Same as the parent [pocket-daw](https://github.com/poealone/pocket-daw) project.
