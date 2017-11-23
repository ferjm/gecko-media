// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

extern crate gecko_media;

use gecko_media::{GeckoMedia, Metadata, PlayerEventSink, PlanarYCbCrImage, TimeStamp};
use std::env;
use std::ffi::CString;
use std::fs::File;
use std::io::prelude::*;
use std::ops::Range;
use std::path::Path;
use std::sync::mpsc;

extern crate time;

fn main() {
    let args: Vec<_> = env::args().collect();
    let filename: &str = if args.len() == 2 {
        args[1].as_ref()
    } else {
        panic!("Usage: test-player file_path")
    };

    let path = Path::new(filename);
    let mime = match path.extension().unwrap().to_str() {
        Some("wav") => "audio/wav",
        Some("mp3") => "audio/mp3",
        Some("flac") => "audio/flac",
        Some("ogg") => "audio/ogg; codecs=\"vorbis\"",
        Some("m4a") => "audio/mp4",
        Some("mp4") => "video/mp4",
        _ => "",
    };
    if mime == "" {
        panic!(
            "Unknown file type. Currently supported: wav, mp3, m4a, flac and ogg/vorbis files.\
                Video files supported: mp4."
        )
    }

    let mut file = File::open(filename).unwrap();
    let mut bytes = vec![];
    file.read_to_end(&mut bytes).unwrap();

    {
        enum Status {
            Error,
            Ended,
            AsyncEvent(CString),
            MetadataLoaded(Metadata),
            BufferedRanges(Vec<Range<f64>>),
            SeekableRanges(Vec<Range<f64>>),
        }
        let (sender, receiver) = mpsc::channel();
        struct Sink {
            sender: mpsc::Sender<Status>,
        }
        impl PlayerEventSink for Sink {
            fn playback_ended(&self) {
                self.sender.send(Status::Ended).unwrap();
            }
            fn decode_error(&self) {
                self.sender.send(Status::Error).unwrap();
            }
            fn async_event(&self, name: &str) {
                self.sender
                    .send(Status::AsyncEvent(CString::new(name).unwrap()))
                    .unwrap();
            }
            fn metadata_loaded(&self, metadata: Metadata) {
                self.sender.send(Status::MetadataLoaded(metadata)).unwrap();
            }
            fn duration_changed(&self, _duration: f64) {}
            fn loaded_data(&self) {}
            fn time_update(&self, _time: f64) {}
            fn seek_started(&self) {}
            fn seek_completed(&self) {}
            fn update_current_images(&self, images: Vec<PlanarYCbCrImage>) {
                for img in images.iter() {
                    let _pixels = img.y_plane.data();
                    let now = TimeStamp(time::precise_time_ns());
                    if img.time_stamp > now {
                        println!("frame display at {} (now is {})", img.time_stamp, now);
                    }
                }
            }
            fn buffered(&self, ranges: Vec<Range<f64>>) {
                self.sender.send(Status::BufferedRanges(ranges)).unwrap();
            }
            fn seekable(&self, ranges: Vec<Range<f64>>) {
                self.sender.send(Status::SeekableRanges(ranges)).unwrap();
            }
        }
        let sink = Box::new(Sink { sender: sender });

        let player = GeckoMedia::get()
            .unwrap()
            .create_blob_player(bytes, mime, sink)
            .unwrap();
        player.play();
        player.set_volume(1.0);
        loop {
            match receiver.recv().unwrap() {
                Status::Ended => {
                    println!("Ended");
                    break;
                }
                Status::Error => {
                    println!("Error");
                    break;
                }
                Status::AsyncEvent(name) => {
                    println!("Event received: {:?}", name);
                }
                Status::MetadataLoaded(metadata) => {
                    println!("MetadataLoaded; duration: {:?}", metadata.duration);
                }
                Status::BufferedRanges(ranges) => {
                    println!("Buffered ranges: {:?}", ranges);
                }
                Status::SeekableRanges(ranges) => {
                    println!("Seekable ranges: {:?}", ranges);
                }
            };
        }
    }

    GeckoMedia::shutdown().unwrap();
}
