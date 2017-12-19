// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

extern crate bindgen;
extern crate cmake;
extern crate walkdir;

use std::env;
use std::path::PathBuf;
use walkdir::WalkDir;

fn compile_bindings() {
    let bindings = bindgen::Builder::default()
        .header("gecko/glue/include/wrapper.hpp")
        .rustified_enum("CanPlayTypeResult")
        .rustfmt_bindings(false)
        .generate()
        .expect("Unable to generate bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}

fn compile_gecko_media() {
    let dst = cmake::build(".");
    println!("cargo:rustc-link-search=native={}", dst.display());
    println!("cargo:rustc-link-lib=static=gecko_media_cmake");

    let target = std::env::var("TARGET").unwrap();

    if let Some(_) = target.find("android") {
        println!("cargo:rustc-link-lib=OpenSLES");
        println!("cargo:rustc-link-lib=dl");
        println!("cargo:rustc-link-lib=stdc++");
    } else if let Some(_) = target.find("darwin") {
        println!("cargo:rustc-link-lib=c++");
        let frameworks = vec![
            "Cocoa",
            "CoreFoundation",
            "CoreAudio",
            "AudioUnit",
            "AudioToolbox",
            "AVFoundation",
            "CoreVideo",
            "VideoToolbox",
        ];
        for framework in &frameworks {
            println!("cargo:rustc-link-lib=framework={}", framework);
        }
    } else if let Some(_) = target.find("linux") {
        println!("cargo:rustc-link-lib=stdc++");
        println!("cargo:rustc-link-lib=dl");
    } else if let Some(_) = target.find("windows") {
        println!("cargo:rustc-link-lib=avrt");
    }

    println!("cargo:rustc-link-lib=lgpllibs");

}

fn print_rerun_if() {
    println!("cargo:rerun-if-changed=CMakeLists.txt");
    for entry in WalkDir::new("gecko") {
        let entry = entry.unwrap();
        println!("cargo:rerun-if-changed={}", entry.path().display());
    }
}

fn main() {
    compile_gecko_media();
    compile_bindings();
    print_rerun_if();
}
