# afscli

AFS archive packer/unpacker — single C++17 file, zero dependencies.

## Usage

```
afscli -e <input.afs> <output_dir>   Extract AFS archive
afscli -c <input_dir> <output.afs>   Create AFS archive
afscli -i <input.afs>                Show AFS information
```

### Extract

```sh
afscli -e LONDON.AFS output/
```

Extracts all entries to `output/` and saves metadata to `output.json`.

### Create

```sh
afscli -c input_dir/ NEW.AFS
```

Reads `input_dir.json` if present (the metadata file produced by extract), so roundtripping works: extract → modify files → repack with original settings preserved. Without metadata, it scans the directory and uses sensible defaults.

### Info

```sh
afscli -i LONDON.AFS
```

Displays header magic, attribute type, alignment, and a table of all entries with names, sizes, custom data, and timestamps.

## Build

```sh
g++ -std=c++17 -O2 -Wall -Wextra -o afscli afscli.cpp
```

Cross-compile for Windows:

```sh
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -static -o afscli.exe afscli.cpp
```

Prebuilt Linux (`afscli`) and Windows x64 (`afscli.exe`) binaries are included in the repo.
