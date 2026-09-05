# DDP Debugger

A JavaFX desktop tool for visualizing and bench-testing [DDP](http://www.3waylabs.com/ddp/)
(Distributed Display Protocol) pixel traffic -- the protocol ServoController and xLights use to
drive pixel controllers over the network. Two tabs:

- **Receiver** -- listens for DDP packets on a UDP port, tracks every distinct sender it sees,
  reassembles each sender's fragmented frame data, and renders it live as a grid of colored boxes.
  A packet log and a per-source diagnostics panel (rate, sizes, malformed/duplicate counts, flag
  histogram) sit below the grid.
- **Sender** -- a bench-test transmitter. Sends solid colors, pure R/G/B, or a marching
  RGBW/RGBWK cycle to any host:port, with a live preview of what's being sent. Handy for
  exercising the Receiver tab (point it at `127.0.0.1`) or a real controller without needing
  xLights running.

Settings (grid size, box size, overlays, listen port, sender config) persist between launches to
a JSON file at `%APPDATA%\DDPDebugger\settings.json`.

## Requirements

- JDK 21 (developed against [Eclipse Temurin 21](https://adoptium.net/)).
- No separate Maven install needed -- use the bundled wrapper (`mvnw.cmd` / `mvnw`).

If a machine already has an older JDK/JRE installed (e.g. a bundled Java 8 from some other tool),
installing Temurin 21 afterward updates the system `JAVA_HOME`/`PATH`, but any terminal that was
already open keeps whatever `JAVA_HOME` it inherited at startup -- environment variable updates
don't reach already-running processes. If `mvnw.cmd javafx:run` fails with `Unsupported class
file major version ...` or `Unrecognized option: --module-path`, that's this: the terminal is
still using an old JDK. Either open a brand new terminal (fully restart it if it's an IDE's
integrated terminal, e.g. quit and reopen VS Code, not just the terminal tab), or override it for
the current session: `set JAVA_HOME=C:\path\to\jdk-21` (cmd) before retrying.

## Running it

Easiest: double-click `run.cmd`, or run `.\run.ps1` in PowerShell. It finds a JDK 17+ install on
its own (checking `JAVA_HOME` first, then common install locations) and launches the app with it
regardless of what an older Java on your `PATH`/`JAVA_HOME` might otherwise cause -- see
Troubleshooting below for why that matters.

Or run Maven directly, if you already know your shell has a good JDK active:

```
mvnw.cmd javafx:run
```

(On macOS/Linux, `./mvnw javafx:run`; the `pom.xml`'s `javafx.platform` property defaults to
`win` -- override it for another OS, e.g. `mvnw.cmd javafx:run -Djavafx.platform=mac`.)

## Building a standalone jar

```
mvnw.cmd package
java -jar target/ddpdebugger-<version>.jar
```

This produces a self-contained "fat" jar with the Windows JavaFX natives bundled in, so it runs
with just a JDK -- no separate JavaFX SDK install needed.

## Packaging as a single Windows executable

Not done yet -- planned options, in order of how much build complexity they add:

1. `jpackage --type app-image`: bundles a trimmed `jlink` runtime with the app into a folder with
   one `.exe` launcher. Not literally one file, but zero-install and easy to zip and hand to
   someone.
2. [Gluon's client/substrate plugin](https://docs.gluonhq.com/) (GraalVM native-image): compiles
   to one true native `.exe` with no bundled JVM. More build complexity and pickier about
   reflection-heavy dependencies -- which is why settings persistence in this project is a
   hand-rolled JSON reader/writer (see `settings/JsonUtil.java`) rather than Gson/Jackson.

macOS/Linux packaging is explicitly out of scope for now.

## Project layout

```
src/main/java/com/xlightsprops/ddpdebugger/
  ddp/       DDP header parsing/building (DdpHeader, DdpPacket, DdpPacketParser, DdpPacketBuilder)
  model/     Per-source state: SourceKey, SourceState, FrameBuffer, PacketStats
  net/       UDP I/O: DdpListener (receiver thread), DdpSender (test-pattern transmitter thread)
  settings/  AppSettings bean + hand-rolled JSON persistence
  ui/        JavaFX views: ReceiverPane, SenderPane, GridCanvas
  Main.java       JavaFX Application
  Launcher.java   Fat-jar entry point (see note in pom.xml about why this exists)
```

## Protocol notes

Header layout, flags, and ID meanings were cross-checked against the primary spec (a local copy
is in `DDP PROTOCOL.html`) and against ServoController's own `ddp_handler.cpp`. A few things worth
knowing if you extend this:

- The data-type byte (RGB/HSL/RGBW encoding) is unreliable in practice -- even the spec's own
  sample code doesn't set it meaningfully. The Receiver/Sender bytes-per-pixel controls are
  manual overrides for this reason, not derived from the packet.
- A source's frame buffer is never implicitly cleared between frames, matching the spec ("the
  buffer is not cleared between display commands") -- a sender can push only the pixels that
  changed.
- If a sender ever sets the Timecode flag, the header grows to 14 bytes. The packet log's Flags
  column will show `TIME` when that happens -- worth checking against any receiver (like
  ServoController today) that assumes a fixed 10-byte header.
