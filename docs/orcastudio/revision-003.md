# OrcaStudio revision 003 — monitoring and workflow tools

## Implemented

- Windows camera viewer can be opened directly, accepts printer host and LAN access code,
  remembers host/window bounds/always-on-top, supports monitor selection, reconnect and PNG snapshots.
  The access code is never saved. Pop-out still passes credentials through stdin, using `--stdin`.
  One instance per normalized host prevents repeated pop-out clicks starting duplicate streams.
  Frame age is visible and turns orange after ten seconds without a frame.
- File > Workflow tools > Save/Apply support recipe. Versioned JSON with only support/raft settings;
  validates the complete prospective configuration before changing the process. Slot assignments are
  deliberately retained from the current plate. Changes are reviewed before application. Existing
  process profiles and 3MF serialization are unchanged.
- Compare slice settings and estimates accepts G-code or sliced 3MF, with a plate picker for multi-plate
  files. Shows changed recorded settings/estimates; missing values are identified. This first version
  does not compare layer geometry or calculate time deltas from differently formatted strings.
- Opt-in successful-send archive. Saves the exact sent G-code/3MF and a JSON record locally, retains
  the newest 100 jobs, and supports result notes. Bambu PrintJob and standard PrintHost upload paths
  are covered. A sent record is not evidence the print finished. Existing device-SD reprints,
  proprietary sending paths that bypass these classes, and failed uploads are not archived.
- CPack explicitly installs the companion viewer, with Start-menu/desktop entries on Windows.

Archive and recipe controls are cross-platform; the pre-existing WinForms companion remains Windows-only.
Nothing in this revision changes generated slicing toolpaths or default print settings.

## Validation

- `g++ -std=c++17 -Wall -Wextra -Werror -I. -Ideps_src tools/workflow_tests/check.cpp -o workflow-check`
  followed by `./workflow-check`: metadata parsing/differences, missing fields, opt-in behavior,
  byte-preserving archive copy, missing source handling, retention and source preservation.
- GitHub `Workflow tools checks`: Linux/Windows archive/parser checks and Windows viewer compilation.
- Full Windows bridge build checks native integration and packaging.
- Hardware/manual checks still required: direct launch and slicer pop-out on a P1P, close slicer while
  viewing, disconnect/reconnect, stale-frame indicator, F11/Esc, multi-monitor restore, snapshots,
  repeated pop-out, recipe round-trip with two different material assignments, invalid recipe rejection,
  archive from P1P and Moonraker sends, notes and multi-plate comparisons. Install and verify the viewer
  shortcut using the resulting installer, not only the portable ZIP.

## Subsequent stages (not implemented in revision 003)

- Synchronized graphical layer comparison.
- Spoolman and additional camera/status backends.
- Experimental layer-time smoothing/support-transition ports: keep defaults unchanged and require
  slicing regression fixtures plus representative physical prints before inclusion in a normal build.
  In particular, preserve the known-good PEBA support-interface process as a baseline.

## Source acknowledgement

The workflow concepts were informed by https://github.com/aceRage/EdgeSlicer (support sets, slice
comparison and sent-file archives). This revision is a small original implementation using OrcaStudio's
existing wxWidgets/configuration and sending APIs; it is not a wholesale EdgeSlicer engine merge.
