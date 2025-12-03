# `tmg` - timer manager

This is a simple CLI program for creating and managing timers which in turn execute arbitrary user commands. It uses a server (daemon) and client architecture both of which are inside a single binary. After starting the daemon using the `-D` flag, the user can use the same binary to [create and manage timers](#Examples).

# Build & Install

## Requirements

- `gcc`
- `libc`

## Install

To build and install the binary into the `$(prefix)/bin` run `make install prefix=<your-prefix>`. By default `prefix` is equal to `~/.local`. If you want to run the daemon as a `systemd` user service, you can place [`misc/tmg.service`](misc/tmg.service) into `~/.config/systemd/user/` and run the following commands to enable the daemon on startup and run it.

```sh
systemd --user daemon-reload
systemd --user enable tmg.service
systemd --user start tmg.service
```

# Examples

Notify the user to take a break in 30 minutes.

```sh
# assuming the tmg daemon is running
tmg -m 30 -R "notify-send 'TAKE A BREAK'" # create a timer

```

List running timers

```sh
tmg -l
# ...
# ... id: 1 ...
# ...
```

Use the `id` to change the timer.

```sh
# actually, take a break in 10 minutes
tmg -c 1 -m 10
```

Delete the timer.

```sh
# breaks are for suckers anyways
tmg -d 1
```

After all is done, close the daemon gracefully.

```sh
tmg -q
```

Some other scripts can be found in the [`misc/scripts`](misc/scripts) directory.
