# Open issues of exeldro/obs-source-record

Status of every issue open upstream on 2026-11-07, against this branch.

## Fixed on this branch

| Issue | Fix |
| --- | --- |
| #164 | Split files keep the spaces of the first file's name (`allow_spaces`). |
| #181, #105, #70, #30 | Two filters that generate the same file name no longer write over each other; the second file gets a ` (2)` suffix. |
| #90 | Split hotkey, button and `record_split` work without automatic splitting; the Split button is no longer greyed out. |
| #42 | The default path follows the OBS output settings (Standard vs FFmpeg recording in Advanced mode). Missing profile values no longer crash. |
| #193, #145 | Save Replay hotkey on the source, named with the plugin's own text. |
| #24, #3 | Replay buffer hotkey bindings are kept when its output is recreated. |
| #38, #135, #141, #40 | The source is taken off the filter's view, and the view's video output dropped, once nothing records. No more extra render time and no reference keeping a deleted source alive. |
| #109 | Outputs stop when the parent source is removed. |
| #170 | Replay buffer memory limit is a setting (was a fixed 10 GB), so long replays work. |
| #178, #75 | Websocket events `record_started`, `record_file_changed`, `record_stopped`, `replay_buffer_started/stopped`, `stream_started/stopped` next to `replay_buffer_saved`. README documents requests and events. |
| #171 | Typed path field, may be relative (portable installs). |
| #32, #150, #155, #116, #67, #23, #113, #158, #87, #31 | Source audio is taken with an audio capture callback instead of the source's Track 1 mix, which libobs only renders while an OBS output uses that track. |
| #84, #91, #92 | An output that stopped on its own (disk full, failed start, lost connection) is recreated, at most every 5 s. |
| #143 | After an OBS video reset (resolution or color format change) the view, encoder and outputs are rebuilt instead of using a freed `video_t`. |
| #102, #100 | `{source}`, `{filter}` and `{recording}` in filename formats. |
| PR #183 | Changing path, container or filename while recording restarts the recording with the new settings. |
| — | Fragmented MP4/MOV are really fragmented (crash safe). |
| — | No 512 byte limit on the recording path. |

## Already fixed upstream (master, after the last release)

#186 (deprecated `obs_properties_add_button`), #191 (new plugin folder structure), #106 (Custom FFmpeg output), #142, #133, #132, #99, #121, #112, #85, #93/#157/#152/#159/#169/#51 (Audio Track All / Custom tracks), #69 (Scale), #39 (frame rate divisor), #80 (start retries capped), #140 and #166 (resize crash, PR #179), #2.

## Not a code problem in this plugin

- Crash elsewhere: #137 (obs-browser), #120 (win-wasapi), #175 (NVENC driver), #163 (encoder too slow: queue over 5 s).
- Configuration or questions: #131, #110, #108, #96, #82, #79, #27, #9, #5, #161 (chapter hotkey and `record_add_chapter` exist), #187 (OBS encoders have no RGB output; use I444).
- Packaging: #160 (Flatpak needs an extension build), #165 and #149 (macOS Gatekeeper), #78, #72, #63.
- Old reports to retest with this build, no usable log: #176, #177, #188, #154, #115, #107, #74, #73, #68, #48, #28, #22, #21, #26, #20, #15, #13, #6, #4, #94, #66, #53, #45, #44, #56, #34, #119, #156, #130, #114, #136, #146, #167.

## Not done

- #148, #122, #153: audio of scenes is still mixed from the children's Track 1 mix; it needs a capture per child.
- #89: recording audio before the source's own filters.
- #12: automatic remux (no remux API outside the OBS UI; use hybrid MP4).
- #64: audio-only sources (the filter records a video view).
- #189: HDR brightness, needs testing on HDR hardware.
- #65: per-filter color format.
- #43: stats dock.
- #127: SRT works through a `srt://` server URL; no separate option.
