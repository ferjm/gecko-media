# Gecko Media Rust Crate

[![Build Status](https://travis-ci.org/servo/gecko-media.svg?branch=master)](https://travis-ci.org/servo/gecko-media)

Firefox's Media stack in a Rust crate.

Build with `cargo build`.

Run tests with `cargo test`.

A minimalistic player is also available: `cargo run --example test-player -- /path/to/some/audio-file.ogg`

## Procedure to update the imported code from Gecko

1. A mozilla-central Gecko build is required. Optimizations and debug should be enabled. Example `mozconfig`:
    ```
    ac_add_options --enable-optimize 
    ac_add_options --enable-debug 
    ```
2. The import script has to be executed like this: `python3 import.py /path/to/mozilla-central gecko/`. The `gecko/src` and `gecko/include` directories content will be removed and the source and header files declared in `data/*.json` will be copied back to those two directories.

3. The import script will create a subfolder inside `glue_diffs/` named with the upstream revision number you are updating to (i.e. glue_diffs/392049:f41930a869a8/). Inside this subfolder you may find a series of diff files. You need to apply these patches manually to the code inside `gecko/glue`. Some context about why this is needed can be found in this [issue](https://github.com/servo/gecko-media/issues/79).

4. Fix potential build errors!
