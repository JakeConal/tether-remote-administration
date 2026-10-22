# Tether v1.0.0 — Binary Quick Start

This archive contains prebuilt Tether executables and ready-to-edit local
configuration files. You do not need CMake, a compiler, or vcpkg to run these
binaries.

Tether has two programs:

- `tether-agent` runs continuously on the machine being administered.
- `tether-console` opens a listener, performs one command, and exits.

The agent dials the console. Between console commands, connection-refused and
redial messages in the agent log are normal because the console is not a
persistent server.

> Authorized use only. Run Tether only on computers you own or are explicitly
> authorized to administer.

## Included files

- `tether-agent` / `tether-agent.exe`
- `tether-console` / `tether-console.exe`
- `agent.json`
- `console.json`
- `tokens.json`
- `LICENSE`
- Windows runtime DLLs where required

## Configuration

The included JSON files are configured for a same-machine test using
`127.0.0.1:47474` and the agent name `agent-lab-1`.

The token in `agent.json` must exactly match the value for `agent-lab-1` in
`tokens.json`. Replace the example token before using Tether across a network.

## Windows quick start

Extract the ZIP, open PowerShell in the extracted directory, and keep two
PowerShell windows open.

In window 1, start the agent:

```powershell
.\tether-agent.exe --config .\agent.json
```

In window 2, list the connected agent:

```powershell
.\tether-console.exe --config .\console.json agents
```

List running processes:

```powershell
.\tether-console.exe --config .\console.json exec agent-lab-1 list
```

List a directory:

```powershell
.\tether-console.exe --config .\console.json exec agent-lab-1 ls "C:\Windows\Temp"
```

Capture a screenshot. `cmd /c` preserves binary output on Windows PowerShell
5.1:

```powershell
cmd /c ".\tether-console.exe --config .\console.json exec agent-lab-1 screenshot > screenshot.png"
```

Capture one webcam frame:

```powershell
.\tether-console.exe --config .\console.json exec agent-lab-1 start_webcam
Start-Sleep -Seconds 3
cmd /c ".\tether-console.exe --config .\console.json exec agent-lab-1 stop_webcam > webcam-frame.jpg"
```

## Linux quick start

Extract the archive and enter its directory:

```sh
tar -xzf tether-v1.0.0-linux-x64.tar.gz
cd tether-v1.0.0-linux-x64
chmod +x tether-agent tether-console
```

In terminal 1, start the agent:

```sh
./tether-agent --config ./agent.json
```

In terminal 2, list the connected agent and running processes:

```sh
./tether-console --config ./console.json agents
./tether-console --config ./console.json exec agent-lab-1 list
```

List a directory and capture the screen:

```sh
./tether-console --config ./console.json exec agent-lab-1 ls /tmp
./tether-console --config ./console.json exec agent-lab-1 screenshot > screenshot.png
```

Linux screen capture requires an accessible X11 display. Camera capture also
requires a supported camera device and permission to access it.

## Expected agent messages

These messages are normal between console invocations:

```text
dial-out attempt failed ... actively refused it
dial-out backed off ...
session down ... console is gone
session down; redialing
```

Every console invocation listens temporarily, waits for the agent to reconnect,
authenticates it, performs one command, and exits. The included console config
waits up to 15 seconds, which covers the agent's maximum 10-second reconnect
backoff.

## Troubleshooting

- If the agent closes immediately, run it from a terminal and read the error.
- If the agent reports an empty token, ensure `agent.json` contains a nonempty
  token.
- If authentication fails, make the token in `agent.json` exactly match the
  `agent-lab-1` value in `tokens.json`.
- If the console reports `unknown agent`, keep the agent running and retry; also
  confirm the address, port, agent name, and firewall settings.
- If the camera is unavailable, close other camera applications and allow
  camera access for desktop programs.
- Avoid testing destructive, keyboard-lock, or keylogging commands until the
  basic connection and read-only commands work correctly.

