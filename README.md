# Source Record filter for OBS Studio

Plugin for OBS Studio to make sources available to record via a filter

# Download

https://obsproject.com/forum/resources/source-record.1285/

# Build
1. In-tree build
    - Build OBS Studio: https://obsproject.com/wiki/Install-Instructions
    - Check out this repository to plugins/source-record
    - Add `add_subdirectory(source-record)` to plugins/CMakeLists.txt
    - Rebuild OBS Studio

1. Stand-alone build (Linux only)
    - Verify that you have package with development files for OBS
    - Check out this repository and run `cmake -S . -B build -DBUILD_OUT_OF_TREE=On && cmake --build build`

# Websocket

Source Record registers the obs-websocket vendor `source-record`. Requests are sent with `CallVendorRequest`.
Every request takes an optional `source` (all sources with a Source Record filter when empty) and `filter`.

| Request | Extra fields |
| --- | --- |
| `record_start` | `filename`, `max_seconds`, `stop_existing` |
| `record_stop` | |
| `record_pause` / `record_unpause` | |
| `record_split` | |
| `record_add_chapter` | `chapter_name` |
| `replay_buffer_start` | `filename`, `stop_existing` |
| `replay_buffer_stop` | |
| `replay_buffer_save` | |
| `stream_start` | `server`, `key`, `stop_existing` |
| `stream_stop` | |

Events are sent as `VendorEvent` with `source`, `filter` and, where it applies, `path`:

| Event | Data |
| --- | --- |
| `record_started` | `path` of the file being written |
| `record_file_changed` | `path` of the new file after a split |
| `record_stopped` | `path` of the last file, `code` |
| `replay_buffer_started` / `replay_buffer_stopped` | `code` on stop |
| `replay_buffer_saved` | `path` of the saved replay |
| `stream_started` / `stream_stopped` | `code` on stop |

# Donations
https://www.paypal.me/exeldro
