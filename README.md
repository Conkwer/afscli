# afscli

AFS archive packer/unpacker — single C++17 file, zero dependencies.

## Usage

### Extract

```sh
afscli -e LONDON.AFS output/
afscli --extract LONDON.AFS output/
```

Extracts all entries to `output/` and saves metadata to `output.json`.

| Flags | Has TOC name | No TOC name |
|---|---|---|
| `-e` / `-e -t` (default) | `01JUCE.EXT` | `00000000` |
| `-e -d` / `-e -t -d` | `01JUCE.EXT` | `00000000.adx` |
| `-e -n` | `00000000` | `00000000` |
| `-e -n -d` | `00000000.EXT` | `00000000.adx` |

- **`-t`, `--toc`** — use TOC filenames when available (default behavior)
- **`-n`, `--numbered`** — force positional numbering for all files
- **`-d`, `--detect`** — detect file types for nameless entries (`.adx`, `.pvr`, `.sfd`, `.ahx`, `.bin`)

### Create

```sh
afscli -c input_dir/ NEW.AFS
afscli --create input_dir/ NEW.AFS
```

Reads `input_dir.json` if present (the metadata file produced by extract), so roundtripping works: extract → modify files → repack with original settings preserved. Without metadata, it scans the directory and uses sensible defaults.

### Info

```sh
afscli -i LONDON.AFS
afscli --info LONDON.AFS
```

Displays header magic, attribute type, alignment, and a table of all entries with names, sizes, custom data, and timestamps.

## Type Detection

With the `-d`/`--detect` flag, nameless entries are classified by magic bytes:

| Magic | Extension | Format |
|---|---|---|
| `0x80 0x00` + `(c)CRI` | `.adx` | CRI ADPCM audio |
| `AHX(` | `.ahx` | CRI MPEG audio |
| `00 00 01 BA` | `.sfd` | Sofdec MPEG-PS video |
| `GBIX`/`PVRT` | `.pvr` | Dreamcast PVR texture |
| *(none matched)* | `.bin` | Unknown binary |

## Build

```sh
g++ -std=c++17 -O2 -Wall -Wextra -o afscli afscli.cpp
```

Cross-compile for Windows:

```sh
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -static -o afscli.exe afscli.cpp
```

ARM64 (Raspberry Pi):

```sh
aarch64-linux-gnu-g++ -std=c++17 -O2 -Wall -Wextra -static -o afscli-arm64 afscli.cpp
```

Prebuilt binaries for all three platforms are included in the repo.
