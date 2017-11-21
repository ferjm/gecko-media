// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

#[allow(unused_extern_crates)]
extern crate cubeb_pulse;
#[allow(unused_extern_crates)]
extern crate encoding_c;
extern crate libc;
extern crate mime;
#[allow(unused_extern_crates)]
extern crate mp4parse_capi;

pub mod mime_parser_glue;
pub mod player;
pub mod timestamp;
mod top;

pub mod bindings {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

#[doc(inline)]
pub use bindings::CanPlayTypeResult as CanPlayType;
pub use player::{Metadata, PlanarYCbCrImage, Plane, Player, PlayerEventSink, Region};
pub use top::GeckoMedia;
pub use timestamp::TimeStamp;

#[cfg(test)]
mod tests {
    use CanPlayType;
    use GeckoMedia;
    use player::{Metadata, PlanarYCbCrImage, Player, PlayerEventSink};
    use std::ffi::CString;
    use std::fs::File;
    use std::io::prelude::*;
    use std::ops::Range;
    use std::sync::mpsc;

    fn test_can_play_type() {
        let gecko_media = GeckoMedia::get().unwrap();
        assert_eq!(gecko_media.can_play_type("bogus/bogus"), CanPlayType::No);
        assert_eq!(
            gecko_media.can_play_type("audio/mp4; codecs=\"bogus\""),
            CanPlayType::No
        );
        assert_eq!(gecko_media.can_play_type("audio/wav"), CanPlayType::Maybe);
        assert_eq!(gecko_media.can_play_type("audio/mp3"), CanPlayType::Maybe);
        assert_eq!(gecko_media.can_play_type("audio/mp4"), CanPlayType::Maybe);
        assert_eq!(gecko_media.can_play_type("audio/flac"), CanPlayType::Maybe);
        assert_eq!(gecko_media.can_play_type("audio/ogg"), CanPlayType::Maybe);
        assert_eq!(
            gecko_media.can_play_type("audio/ogg; codecs=\"vorbis\""),
            CanPlayType::Probably
        );
        assert_eq!(gecko_media.can_play_type("video/mp4"), CanPlayType::Maybe);
    }

    enum Status {
        Error,
        Ended,
        AsyncEvent(CString),
        MetadataLoaded(Metadata),
        DurationChanged(f64),
        LoadedData,
        TimeUpdate(f64),
        SeekStarted,
        SeekComplete,
        UpdateImages(Vec<PlanarYCbCrImage>),
        Buffered(Vec<Range<f64>>),
        Seekable(Vec<Range<f64>>),
    }
    fn create_test_player(path: &str, mime: &str) -> (Player, mpsc::Receiver<Status>) {
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
            fn duration_changed(&self, duration: f64) {
                self.sender.send(Status::DurationChanged(duration)).unwrap();
            }
            fn loaded_data(&self) {
                self.sender.send(Status::LoadedData).unwrap();
            }
            fn time_update(&self, time: f64) {
                self.sender.send(Status::TimeUpdate(time)).unwrap();
            }
            fn seek_started(&self) {
                self.sender.send(Status::SeekStarted).unwrap();
            }
            fn seek_completed(&self) {
                self.sender.send(Status::SeekComplete).unwrap();
            }
            fn update_current_images(&self, images: Vec<PlanarYCbCrImage>) {
                self.sender.send(Status::UpdateImages(images)).unwrap();
            }
            fn buffered(&self, ranges: Vec<Range<f64>>) {
                self.sender.send(Status::Buffered(ranges)).unwrap();
            }
            fn seekable(&self, ranges: Vec<Range<f64>>) {
                self.sender.send(Status::Seekable(ranges)).unwrap();
            }
        }
        let sink = Box::new(Sink { sender: sender });
        let mut file = File::open(path).unwrap();
        let mut bytes = vec![];
        file.read_to_end(&mut bytes).unwrap();
        let player = GeckoMedia::get()
            .unwrap()
            .create_blob_player(bytes, mime, sink)
            .unwrap();
        player.set_volume(0.001);
        (player, receiver)
    }

    fn test_basic_playback() {
        let (player, receiver) = create_test_player("gecko/test/audiotest.wav", "audio/wav");
        player.play();
        let ok;
        loop {
            match receiver.recv().unwrap() {
                Status::Ended => {
                    ok = true;
                    break;
                }
                Status::Error => {
                    ok = false;
                    break;
                }
                _ => {}
            };
        }
        assert!(ok);
    }

    fn test_seeking() {
        let (player, receiver) = create_test_player("gecko/test/audiotest.wav", "audio/wav");
        let mut duration = 0.0;
        let mut current_time = 0.0;
        let mut reached_metadata_loaded = false;
        let mut reached_loaded_data = false;
        let mut reached_seek_started = false;
        let mut reached_seek_complete = false;
        let mut reached_ended = false;
        let mut reached_error = false;
        let mut reached_buffered = false;
        let mut reached_seekable = false;
        loop {
            match receiver.recv().unwrap() {
                Status::MetadataLoaded(metadata) => {
                    reached_metadata_loaded = true;
                    duration = metadata.duration;
                    assert!(duration > 0.0);
                }
                Status::LoadedData => {
                    reached_loaded_data = true;
                    assert!(reached_metadata_loaded);
                    player.seek(duration / 2.0);
                }
                Status::TimeUpdate(time) => {
                    current_time = time;
                }
                Status::SeekStarted => {
                    reached_seek_started = true;
                }
                Status::SeekComplete => {
                    reached_seek_complete = true;
                    assert!(reached_seek_started);
                    // The current time should have updated to approximately
                    // the seek target of (duration / 2.0).
                    let delta = (current_time - duration / 2.0).abs();
                    assert!(delta < 0.0001);
                    player.play();
                }
                Status::Ended => {
                    reached_ended = true;
                    break;
                }
                Status::Error => {
                    reached_error = true;
                    break;
                }
                Status::UpdateImages(_images) => {
                }
                Status::Buffered(_ranges) => {
                    reached_buffered = true;
                }
                Status::Seekable(_ranges) => {
                    reached_seekable = true;
                }
                _ => {}
            };
        }
        assert!(reached_metadata_loaded);
        assert!(reached_loaded_data);
        assert!(reached_seek_started);
        assert!(reached_seek_complete);
        assert!(reached_ended);
        assert!(!reached_error);
        assert!(reached_buffered);
        assert!(reached_seekable);
    }

    #[test]
    fn run_tests() {
        GeckoMedia::get().unwrap();
        test_can_play_type();
        GeckoMedia::get().unwrap().test();
        let (sender, receiver) = mpsc::channel();
        GeckoMedia::get().unwrap().queue_task(
            move || sender.send(()).unwrap(),
        );
        receiver.recv().unwrap();
        test_basic_playback();
        test_seeking();
        GeckoMedia::shutdown().unwrap();
    }
}
