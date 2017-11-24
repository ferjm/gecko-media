// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

macro_rules! def_gecko_media_struct (
    ($struct:ident) => (
        use GeckoMedia;
        pub struct $struct {
            gecko_media: GeckoMedia,
            id: usize,
        }
    );
);

macro_rules! impl_new_gecko_media_struct (
    ($struct:ident, $callbacks:ident, $create:ident) => (
        use bindings::$create;
        impl $struct {
            pub fn new(gecko_media: GeckoMedia,
                       id: usize,
                       callbacks: Box<$callbacks>) -> $struct {
                let callbacks = to_ffi_callbacks(callbacks);
                gecko_media.queue_task(move || unsafe {
                    $create(id, callbacks);
                });
                $struct {
                    gecko_media,
                    id,
                }
            }
        }
    );
);

macro_rules! impl_drop_gecko_media_struct (
    ($struct:ident, $shutdown:ident) => (
        use bindings::$shutdown;
        impl Drop for $struct {
            fn drop(&mut self) {
                let id = self.id;
                self.gecko_media.queue_task(move || unsafe {
                    $shutdown(id);
                });
            }
        }
    );
);

macro_rules! def_gecko_callbacks_ffi_wrapper (
    ($callbacks:ident) => (
        use std::os::raw::c_void;
        struct Wrapper {
            callbacks: Box<$callbacks>,
        }
        unsafe extern "C" fn free(ptr: *mut c_void) {
            drop(Box::from_raw(ptr as *mut Wrapper));
        }
    );
);
