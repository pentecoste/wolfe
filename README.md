# Wolfe

An audio patchbay for Windows: inputs, applications, effects and outputs are
nodes, and you decide what is connected to what. The idea is what qpwgraph or
Carla give you on Linux, built on what Windows offers (WASAPI).

There are two programs on the same engine: `wolfe.exe`, a window with the
patch canvas that keeps running in the notification area, and `wolfe-cli.exe`,
which runs a graph described in a JSON file from a console.

## Build

Needs the MSYS2 UCRT64 toolchain (g++) and CMake on the PATH.

    cmake -S . -B build -G "MinGW Makefiles"
    cmake --build build -j 8

This produces, in `build/`:

- `wolfe.exe`: the program
- `wolfe-cli.exe`: the same engine from a console
- `wolfe-enginetest.exe`, `wolfe-controllertest.exe`: checks that need no audio device
- `wolfe-sigtool.exe`: plays test signals and records devices, for measuring

## The window

Every recording device, playback device and application that is using audio
is a box on the canvas. Nothing is opened just for being shown: a device is in
use only while something is connected to it.

Audio comes out of a box on the right and goes in on the left. Every box has
an output pin: what a device records, what an application plays, what an
effect makes, what is being played on a playback device. Effects and playback
devices have an input pin, and so does an application while it is recording.

- Drag from a pin to another pin to connect them. A pin takes any number of
  links: drag from it again to send the same thing somewhere else as well, or
  onto it again to add something to what it already gets.
- Drag a link itself to move it: the end nearer to where you took hold comes
  off, and goes onto the pin you let go on. Pulled well away and let go over
  nothing, the link is removed; so it is with Ctrl+click, or by clicking it
  and pressing Delete.
- Right-click a link for its menu (gain, remove), a box for its own, the
  canvas (or the Add menu) to add an effect. Effects and boxes for things
  that are no longer there can be removed (Delete).
- Middle button, or Alt + drag, moves the view. Hover a box or a link for
  details.

### Two kinds of link

Windows itself connects every application to one playback device, and to one
recording device while it records. Those links are drawn pale, and fainter
still while nothing is flowing: such a link shows where the application will
play the next time it does. Windows carries that audio, with no delay, and
Wolfe changes them by asking Windows to:

- drag an application's own link onto another playback device, and the
  application moves there;
- remove it, and the application is not heard on any device any more (see the
  silent device, below); connect the application to a device again and it is
  back;
- drag a recording device onto an application that records, and it records
  from that one instead.

Links drawn bright are carried by Wolfe, which captures the source and plays
it on the destination. There can be any number of them, between any two boxes
that make sense, each with its own gain; they arrive a little later than the
original (see the limits). So a second link drawn from an application that is
already playing somewhere is a copy: it now plays in both places.

The two kinds work together the way one would expect of plain links. To have
an application on the speakers instead of the headphones, either drag its link
across, or connect it to the speakers and then remove the link to the
headphones: a copy that has no gain set becomes the application's own link as
soon as the old one is gone. And if Windows starts playing an application on a
device Wolfe was already carrying a copy of it to, the copy is paused (drawn
washed out) for as long as that lasts, so that nothing is heard twice.

Anything that is not a recording device (an effect, another application, what
is played on a device) reaches an application that records through a virtual
cable. Drag it onto the application: Wolfe puts the link into the playback end
of the cable the application is recording from, or of a cable nothing else is
using, and asks Windows to make the application record from the other end.
The canvas then shows every step: the bright link into the cable, the two
ends of the cable (recognised by name: VB-Audio and a few others), the
application.

Windows can also play a recording device on a playback device by itself:
"Listen to this device", in the properties of the recording device. On the
canvas that is a pale link from the one to the other, and it is handled like
the rest: drag it to another playback device to move it, remove it to switch
it off. Its menu hands it over to Wolfe, which gives it a gain and less
delay; the menu of a link of Wolfe's between a recording and a playback
device hands that one to Windows, which goes on carrying it while Wolfe is
closed. A recording device can have one such link.

An application follows these requests at once when it uses Windows' default
device, as most do. Some only notice the next time they open their audio, and
one that has a device chosen in its own settings ignores them: Wolfe says so
when a request has had no effect, and the canvas keeps showing what is really
connected.

### The silent device

Windows always plays an application on some device. Connected to nothing
therefore means playing on a device that nobody listens to. Wolfe uses Steam's
streaming device for that when there is one; any playback device can be given
the job in the Wolfe menu, or by right-clicking it. Links to it are not drawn.

An application taken off every device is only heard through links carried by
Wolfe. So when Wolfe exits it is given back to Windows' default device, and
taken off again the next time Wolfe runs. A move from one device to another
is a Windows setting like any made in Settings > System > Sound > Volume
mixer, and stays as it is with or without Wolfe. "Let Windows choose its
devices again", in an application's menu, undoes either.

### What a box says

`live` when Wolfe is using it, `in use` when only Windows is, `idle`
otherwise; `heard nowhere` for an application taken off every device. The bar
is the level as Windows measures it, for every device and every application
whether or not Wolfe is involved (a recording device only has a level while
something records from it). A box with an orange outline is connected but not
working: its text says why (the application is not running, the device is not
plugged in...).

The graph is saved by itself in `%APPDATA%\Wolfe\graph.json` and loaded again
at the next start. Closing the window leaves Wolfe running in the notification
area (the Wolfe menu can turn that off, and can make it start with Windows);
Exit is in that menu and on the icon. What the engine reports is written to
`%APPDATA%\Wolfe\wolfe.log`.

    wolfe.exe                    open the window
    wolfe.exe --tray             start hidden in the notification area
    wolfe.exe --import <file>    replace the graph with the one in a file
                                 (a .json file given without --import does the same)
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
        { "id": "mic",         "type": "input",   "device": "Razer Seiren Mini" },
        { "id": "denoise",     "type": "rnnoise" },
        { "id": "soundrio",    "type": "app",     "process": "\\Soundrio\\" },
        { "id": "virtual_mic", "type": "output",  "device": "CABLE Input" }
      ],
      "links": [
        { "from": "mic",      "to": "denoise" },
        { "from": "denoise",  "to": "virtual_mic" },
        { "from": "soundrio", "to": "virtual_mic", "gain_db": -3 }
      ]
    }

The window writes the same format, adding where each box is drawn. Only what
Wolfe carries is in the file: the device an application plays on is Windows'
to remember. (A link drawn from a playback device is written as a `loopback`
node for that device; an application kept off every device has
`"parked": true`.) Comments (`//`) are allowed when writing one by hand. Node
types:

| type       | what it is                                             | needs     |
|------------|--------------------------------------------------------|-----------|
| `input`    | a recording device (microphone, far end of a cable)    | `device`  |
| `loopback` | everything being played on a playback device           | `device`  |
| `app`      | everything one application is playing                  | `process` |
| `output`   | a playback device (headphones, near end of a cable)    | `device`  |
| `mix`      | a junction: the sum of its inputs, unchanged           |           |
| `rnnoise`  | noise suppression for voice (its output is mono)       |           |

`device` is `default`, or any part of the device name as `wolfe-cli devices` shows
it. `process` is an executable name (`chrome.exe`) or part of its full path
(`\\Soundrio\\`); the path form is how to single out one Java program. Inputs
and outputs that are missing (device unplugged, application not running) are
retried every second.

An application is often several processes (a launcher that starts a command
prompt that starts the real program). Windows captures one process together
with the processes it started directly, and nothing further down. Wolfe looks
at the whole family and captures the process that is actually playing, and
follows it if that changes; the report shows which one it picked.

Several links into the same node are added together. `gain_db` on a link is
optional. `"margin_ms"` at the top level (default 4) is extra delay given to
every source; raise it if the report shows `late` counts that keep growing.

## Reading the report

    in  mic          capturing Microfono (Razer Seiren Mini)   peak -41.4 dB
    out virtual_mic  playing on CABLE Input (...)               peak -49.7 dB  queued 15.0 ms (lowest 5.0)  longest wait 11.5 ms  underruns 0
          mic          age 24.0 ms (-0.01)  rate +8.6 ppm  spare 7.7 ms  late 0  jumps 0

For each output: how much audio is kept queued in the device, and how many
times the device ran out (`underruns`). An output that runs out queues 10 ms
more from then on, so this should stop growing after the first seconds.

For each source feeding that output: `age` is how old the audio is when it is
handed to the output (and how far it currently is from that), `rate` is how
much the source's clock differs from the output's, `late` counts frames that
arrived too late and were replaced by silence, `jumps` counts times the
reading position had to skip.

## How it works

- Everything inside the graph is 48 kHz stereo float. Windows converts at the
  edge for devices that run at another format.
- Each input has a thread that copies what it captures into a buffer, as a
  timeline. Each output has a thread paced by its device: every time the
  device takes 10 ms of audio, the part of the graph that leads to that
  output is computed for 10 ms.
- Two devices never agree exactly on how long a second is. Each output reads
  each source through a resampler whose ratio is adjusted continuously (a few
  parts per million) so that the two neither drift apart nor run dry.
- Delays that depend on the hardware (how much to queue in an output, how far
  behind to read a source) start at the minimum and grow when the device
  turns out to need more.

## Limits that come from Windows

- Virtual devices cannot be created by a program: a "virtual microphone" has
  to be a virtual cable installed beforehand (VB-CABLE or similar). Wolfe uses
  the ones that exist, and each different mix sent into an application needs
  a cable of its own.
- An application has no input of its own: audio only reaches it through the
  device it records from. Likewise it plays on one device at a time; the
  second and further places it is heard in are copies carried by Wolfe.
- Moving an application, and choosing what it records from, use the same
  per-application setting as Windows' own sound settings. The way in is not
  documented by Microsoft; Wolfe uses it on Windows 10 21H2 and later. On
  anything older only the links carried by Wolfe are available.
- Windows remembers that setting by executable: two programs started by the
  same executable (two Java programs on one Java) share it.
- "Listen to this device" is switched the way Windows' Sound control panel
  switches it, which is not documented either. Wolfe first reads the setting
  both that way and the documented way, and changes nothing if they differ.
- A program that listens to a playback device (a screen recorder, a game
  streaming host) is not shown: Windows lists it exactly like a program that
  plays on that device. What it hears is whatever goes into that device.
- An application muted in the Windows volume mixer is captured as silence.
- If an application plays audio from processes in different branches of its
  family, only the branch with the most of them is captured.
- Routing through Wolfe adds delay: about 60 ms for an application sent to a
  hardware output, as measured on the development machine.

## Third-party code

- RNNoise v0.1.1 (`third_party/rnnoise`), BSD 3-Clause, see `COPYING` there.
- Dear ImGui 1.92.9b (`third_party/imgui`), MIT.
- imnodes, commit eb36902 (`third_party/imnodes`), MIT.
- JSON for Modern C++ 3.11.3 (`third_party/nlohmann/json.hpp`), MIT.
