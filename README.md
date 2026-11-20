# Wolfe

An audio patchbay for Windows: microphones, speakers and gains are boxes,
and you decide what goes where. The idea is what qpwgraph or Carla give you
on Linux, built on what Windows offers.

Wolfe is one big node of its own. It has playback and recording devices of
its own, which Windows lists like any sound card. Which applications play on
them or record from them is chosen in Windows' settings; inside Wolfe they
are boxes like your microphone and your headphones, and you link them as you
like.

There are two programs on the same engine: `wolfe.exe`, a window with the
patch canvas that keeps running in the notification area, and `wolfe-cli.exe`,
which runs a graph described in a JSON file from a console.

## What it needs

- [usbip-win2](https://github.com/vadimgrn/usbip-win2), for Wolfe's own
  devices. Its driver is signed by Microsoft; installing it needs an
  administrator once, using it does not. Wolfe offers its devices over USB/IP
  on 127.0.0.1 (port 3241) and has usbip-win2 attach them: to Windows they
  are USB sound cards. Without it everything else works, but Wolfe has no
  devices of its own.
- Another USB/IP server on the machine (usbipd-win) is fine: it uses port 3240.

## Build

Needs the MSYS2 toolchain (g++) and CMake on the PATH.

    cmake -S . -B build -G "MinGW Makefiles"
    cmake --build build -j 8

This produces, in `build/`:

- `wolfe.exe`: the program
- `wolfe-cli.exe`: the same engine from a console
- `wolfe-enginetest.exe`, `wolfe-controllertest.exe`: checks that need no audio device
- `wolfe-sigtool.exe`: plays test signals and records devices, for measuring

## The window

The canvas shows only what you put on it. **Ctrl+A** opens a search for
something to add: type part of its name, and Enter adds the one highlighted
(Up and Down choose another). The Add menu, and a right click on the canvas,
offer the same things.

| box              | what it is                                                                          | pin   |
|------------------|-------------------------------------------------------------------------------------|-------|
| Microphone       | a recording device of the computer (a microphone, the far end of a virtual cable)  | out   |
| Wolfe speaker    | a playback device of Wolfe's own: what applications play on it comes into Wolfe     | out   |
| Gain             | passes on what is linked into it, louder or quieter                                 | both  |
| Speaker          | a playback device of the computer (headphones, speakers)                           | in    |
| Wolfe microphone | a recording device of Wolfe's own: applications that record from it hear what is linked into it | in |

Wolfe takes audio from microphones and from its own speakers, and sends it to
speakers and to its own microphones. It does not show applications and does
not move them: give an application one of Wolfe's devices in Windows'
settings (Settings > System > Sound > Volume mixer) or in the application's
own, and link that device in Wolfe. Windows shows Wolfe's microphones as line
inputs, so that it does not apply its voice processing to them.

- Drag from a pin to another pin to link them. A pin takes any number of
  links: links into the same box are added together, and a box can feed any
  number of others. A link that would bring audio back to where it came from
  is refused, also through the two ends of a virtual cable.
- Drag a link itself to move it: the end nearer to where you took hold comes
  off, and goes onto the pin you let go on. Pulled well away and let go over
  nothing, the link is removed; so it is with Ctrl+click, or by clicking it
  and pressing Delete.
- A gain's value is dragged left or right; a double click lets you type one,
  from -100 to +40 dB.
- Right-click a box or a link for its menu. Delete removes what is selected.
- Middle button, or Alt + drag, moves the view. Hover a box or a link for
  details.

Removing one of Wolfe's devices is asked first: Windows loses it, and sends
the applications that were given it back to the default device. A new device
with the same name is a new device to Windows. Wolfe's devices exist while
Wolfe runs, whether or not anything is linked to them; a device of the
computer is only opened while something is linked to it.

### What a box says

`live` when Wolfe is using it, `not linked` when nothing is linked to it,
`not plugged in` when the device is missing (the box stays, and works again
when the device comes back). One of Wolfe's devices is `ready` once Windows
has it, and a microphone of Wolfe's is `in use` while an application records
from it. The bar is the level as Windows measures it (a recording device only
has a level while something records from it). A box with an orange outline is
linked but not working: its text says why. When a link cannot be made, the
reason appears next to the mouse.

The graph is saved by itself in `%APPDATA%\Wolfe\graph.json` and loaded again
at the next start. Closing the window leaves Wolfe running in the notification
area (the Wolfe menu can turn that off, and can make it start with Windows);
Exit is in that menu and on the icon. What the engine reports is written to
`%APPDATA%\Wolfe\wolfe.log`.

    wolfe.exe                    open the window
    wolfe.exe --tray             start hidden in the notification area
    wolfe.exe --file <file>      keep the graph in that file instead
    wolfe.exe --selftest <file>  try the mouse and keyboard gestures by itself
                                 and write what happened to a file

## The console program

    wolfe-cli devices                 list the audio devices
    wolfe-cli run examples/soundrio.json

`run` keeps going until Ctrl+C and prints the state of the graph every 5
seconds (`--report N` changes that, `--seconds N` makes it stop by itself).
Do not run it on the same devices as the window at the same time: the audio
would arrive twice.

## The graph file

    {
      "nodes": [
        { "id": "music",  "type": "virtual_speaker", "name": "Wolfe Music" },
        { "id": "chat",   "type": "virtual_mic",     "name": "Wolfe Chat" },
        { "id": "mic",    "type": "input",           "device": "Razer Seiren Mini" },
        { "id": "quiet",  "type": "gain",            "gain_db": -12 },
        { "id": "phones", "type": "output",          "device": "Headphones" }
      ],
      "links": [
        { "from": "mic",   "to": "chat" },
        { "from": "music", "to": "phones" },
        { "from": "music", "to": "quiet" },
        { "from": "quiet", "to": "chat" }
      ]
    }

The window writes the same format, adding where each box is drawn. Comments
(`//`) are allowed when writing one by hand. Node types:

| type              | what it is                                  | needs    |
|-------------------|---------------------------------------------|----------|
| `virtual_speaker` | one of Wolfe's playback devices             | `name`   |
| `virtual_mic`     | one of Wolfe's recording devices            | `name`   |
| `input`           | a recording device                          | `device` |
| `output`          | a playback device                           | `device` |
| `gain`            | louder or quieter: `gain_db`, 0 if left out |          |

`device` is `default`, or any part of the device name as `wolfe-cli devices`
shows it. Devices that are missing are retried every second.

Wolfe's devices are known to Windows by a serial number: Windows remembers by
it which applications were given the device. The window gives each new device
one and writes it in the file (`"serial"`); a hand-written node without one
gets one made from its id. Keep both as they are once applications use the
device.

Several links into the same node are added together. `"margin_ms"` at the
top level (default 4) is extra delay given to every source; raise it if the
report shows `late` counts that keep growing.

The console program also knows a few things the window does not offer:
`loopback` nodes (everything played on a playback device, `device`), `app`
nodes (everything one application plays, `process`), `mix` and `rnnoise`
(noise suppression for voice) nodes, and `gain_db` on a link. In a file the
window loads, `mix` and `rnnoise` nodes become gains at 0 dB (so that their
links stay), links lose their gain, and the rest is left out; its log says
what was changed.

## Reading the report

    in  mic          capturing Microfono (Razer Seiren Mini)   peak -41.4 dB
    in  music        Wolfe's speaker "Wolfe Music": ready  peak -12.0 dB
          restarts 0  jumps 0  missing 0  late max 1.4 ms
          loopback: capturing Altoparlanti (Wolfe Music)
      into the graph:
          mic          age 24.0 ms (-0.01)  rate +8.6 ppm  spare 7.7 ms  late 0  jumps 0
    out phones       playing on Headphones (...)   peak -12.0 dB  queued 15.0 ms (lowest 5.0)  longest wait 11.5 ms  underruns 0
      "phones" keeps the graph's time

For each source coming into the graph: `age` is how old the audio is when the
graph takes it (and how far it currently is from that), `rate` is how much the
source's clock differs from the graph's, `late` counts frames that arrived too
late and were replaced by silence, `jumps` counts times the reading position
had to skip.

For each output: how much audio is kept queued in the device, and how many
times it ran out (`underruns`). An output that runs out queues 10 ms more from
then on, so this should stop growing after the first seconds.

For each of Wolfe's devices: how often its stream restarted, jumped to catch
up with the graph, or (a microphone) found audio missing.

## How it works

- Everything inside the graph is 48 kHz stereo float, computed in cycles of
  10 ms, each node once per cycle whatever the number of places it goes to.
- One playback device leads: the graph computes a cycle whenever that device
  needs one, and the device plays it as it is. While none does, a timer keeps
  the pace.
- Every other device has a clock of its own, and never agrees exactly with
  the graph on how long a second is. Each source is read through a resampler
  whose ratio is adjusted continuously so that the two neither drift apart
  nor run dry; the same at each output that does not lead.
- Wolfe's own devices need none of that: they keep the graph's time, as a
  sound card keeps its clock's. A speaker tells Windows how fast to send, a
  microphone sends as fast as the graph makes audio.
- What is played on a Wolfe speaker comes into the graph through Windows'
  loopback of that speaker, which hands it over about 20 ms sooner than the
  USB stream does; the stream takes over if the loopback stops (an
  application that has the speaker to itself, in exclusive mode).
- Delays that depend on the hardware (how much to queue in an output, how far
  behind to read a source) start at the minimum and grow when the device
  turns out to need more.

Measured on the development machine, from the moment audio is played to the
moment it comes out of the other end: an application on a Wolfe speaker to a
hardware output, 35-40 ms; into an application recording from a Wolfe
microphone, 85-100 ms, of which 60-70 ms are spent inside Windows between the
device and the application.

## Limits that come from Windows

- Windows connects an application to one playback device at a time and one
  recording device at a time, chosen in its settings or in the application.
  Wolfe does not change that choice; it gives applications devices worth
  choosing.
- Windows makes a new device the default one when it first appears, and
  again every time it comes back while it is the default Windows remembers:
  a Wolfe microphone would then be the default every time Wolfe starts.
  Choose your own device as the default once (Settings > System > Sound) and
  Windows remembers that instead.
- An application muted in the Windows volume mixer is silent on Wolfe's
  devices too.

## Third-party code

- RNNoise v0.1.1 (`third_party/rnnoise`), BSD 3-Clause, see `COPYING` there.
- Dear ImGui 1.92.9b (`third_party/imgui`), MIT.
- imnodes, commit eb36902 (`third_party/imnodes`), MIT.
- JSON for Modern C++ 3.11.3 (`third_party/nlohmann/json.hpp`), MIT.
