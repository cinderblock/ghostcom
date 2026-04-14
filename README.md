# GhostCOM

Virtual COM port creation for Node.js and Bun on Windows.

Create fake serial ports that appear as real COM ports to the operating system. External applications can open and use them as if they were physical serial devices, while your Node.js/Bun code controls the other end through a high-performance native duplex stream.

## Features

- **Real COM ports** — Ports appear in Device Manager and are discoverable by any Windows application
- **High-performance native streams** — Zero-copy data path via overlapped I/O on a kernel driver, surfaced as a Node.js `Duplex` stream
- **Byte-agnostic** — Data flows at native speed regardless of baud rate, parity, or other serial line settings
- **Tappable control signals** — Observe every serial configuration change (baud rate, DTR/RTS, flow control, etc.) as events
- **Null-modem crossover** — Companion-side DTR/RTS appear as DSR+DCD / CTS on the COM side
- **Node.js & Bun** — Works with both runtimes via N-API

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Your Application (Node.js / Bun)                           │
│                                                             │
│  const port = await createPort({ portNumber: 10 });         │
│  port.stream.pipe(port.stream);  // echo                    │
│  port.on("signal", (s) => console.log(s.baudRate));         │
└──────────────┬──────────────────────────┬───────────────────┘
               │ Duplex stream            │ Signal events
               ▼                          ▼
┌─────────────────────────────────────────────────────────────┐
│  Rust Native Addon (napi-rs)                                │
│  Overlapped I/O threads + ThreadsafeFunction callbacks      │
└──────────────┬──────────────────────────┬───────────────────┘
               │ ReadFile/WriteFile       │ DeviceIoControl
               ▼                          ▼
┌─────────────────────────────────────────────────────────────┐
│  KMDF Kernel Driver                                         │
│  \\.\GCOM<N>           ←──ring buffers──→  \\.\COM<N>       │
│  (companion device)     (signal state)     (serial port)    │
└──────────────────────────────────────────┬──────────────────┘
                                           │
                                           ▼
                              External application
                              (PuTTY, firmware tools, etc.)
```

## Quick Start

```ts
import { createPort, SignalChanged } from "ghostcom";

// Create a virtual COM port
const port = await createPort({ portNumber: 10 });
console.log(`Created ${port.portName}`); // "COM10"

// Respond to data from external applications
port.stream.on("data", (chunk) => {
  console.log("Received:", chunk);
  port.stream.write(Buffer.from("ACK\r\n"));
});

// Monitor serial configuration changes
port.on("signal", (state) => {
  if (state.changedMask & SignalChanged.BAUD) {
    console.log(`Baud rate changed to ${state.baudRate}`);
  }
});

// Know when an app opens/closes the port
port.on("com-open", () => {
  console.log("External application connected");
  port.setSignals({ dtr: true, rts: true }); // Assert DSR+CTS
});

// Clean up
await port.destroy();
```

## Prerequisites

### Driver Installation

GhostCOM requires a kernel-mode driver to create real COM port devices. The driver must be built with the Windows Driver Kit and installed with administrator privileges.

**Requirements:**

1. **Visual Studio 2022** with the "Desktop development with C++" workload
2. **Windows SDK** (included with VS)
3. **Windows Driver Kit (WDK)** — [Download](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk)
4. **Test signing enabled** (for development):
   ```powershell
   bcdedit /set testsigning on
   # Reboot required
   ```

**Build and install the driver:**

```powershell
# Build
bun run build:driver

# Self-sign for testing
bun run build:driver -- -Sign

# Install (requires Administrator)
bun run install:driver
```

### Native Addon

The Rust native addon requires:

1. **Rust toolchain** — [Install via rustup](https://rustup.rs/)
2. **MSVC target**: `rustup target add x86_64-pc-windows-msvc`

```bash
bun install
bun run build:addon
bun run build:ts
```

## API

### `createPort(options?): Promise<VirtualPort>`

Create a new virtual COM port.

| Option | Type | Description |
|--------|------|-------------|
| `portNumber` | `number` | COM port number (e.g., 10 for COM10). Auto-assigned if omitted. |

### `VirtualPort`

| Property | Type | Description |
|----------|------|-------------|
| `portName` | `string` | Port name, e.g. `"COM10"` |
| `portNumber` | `number` | Port number |
| `stream` | `Duplex` | Bidirectional byte stream |
| `signals` | `SignalState` | Last known signal state snapshot |
| `isComOpen` | `boolean` | Whether an external app has the port open |
| `destroyed` | `boolean` | Whether the port has been destroyed |

| Method | Description |
|--------|-------------|
| `setSignals({ dtr?, rts? })` | Set companion output signals (DTR→DSR+DCD, RTS→CTS) |
| `destroy(): Promise<void>` | Tear down the port and remove it from the system |

| Event | Payload | Description |
|-------|---------|-------------|
| `"signal"` | `SignalState` | Any serial configuration change |
| `"com-open"` | — | External app opened the COM port |
| `"com-close"` | — | External app closed the COM port |
| `"error"` | `Error` | Unrecoverable error |

### `SignalState`

```ts
interface SignalState {
  sequenceNumber: number;
  changedMask: number;    // Use SignalChanged.* constants
  baudRate: number;
  dataBits: 5 | 6 | 7 | 8;
  stopBits: "one" | "one-five" | "two";
  parity: "none" | "odd" | "even" | "mark" | "space";
  dtr: boolean;
  rts: boolean;
  breakState: boolean;
  handflow: HandFlow;
  specialChars: SpecialChars;
  waitMask: number;
}
```

### `SignalChanged` Constants

```ts
SignalChanged.BAUD         // Baud rate changed
SignalChanged.LINE_CONTROL // Data bits, stop bits, or parity changed
SignalChanged.DTR          // DTR asserted or cleared
SignalChanged.RTS          // RTS asserted or cleared
SignalChanged.BREAK        // Break state changed
SignalChanged.HANDFLOW     // Flow control configuration changed
SignalChanged.CHARS        // Special characters changed
SignalChanged.WAIT_MASK    // WaitCommEvent mask changed
SignalChanged.COM_OPEN     // External app opened the COM port
SignalChanged.COM_CLOSE    // External app closed the COM port
```

### Utility Functions

```ts
listPorts(): PortInfo[]          // List active virtual ports
isDriverInstalled(): boolean     // Check if the driver is loaded
driverVersion(): string | null   // Get driver version
```

## Examples

```bash
# Echo server — echoes all data back to the sender
bun run examples/echo.ts 10

# Signal monitor — log all serial config changes
bun run examples/signal-monitor.ts 20

# Virtual null-modem pair — bridge two ports together
bun run examples/pair.ts 10 11
```

## How It Works

### Data Flow

1. External app writes bytes to COM10 via `WriteFile`
2. The kernel driver's COM-side write handler copies bytes into the **ComToCompanion** ring buffer (64KB, lock-free SPSC)
3. If the companion has a pending read, the driver immediately satisfies it from the ring buffer
4. The Rust addon's read thread (blocked on overlapped `ReadFile`) wakes up with the data
5. The read thread invokes a `ThreadsafeFunction` to push the `Buffer` onto the JS event loop
6. `VirtualPortStream.push(chunk)` delivers the data to your stream consumer

The reverse direction (your code → external app) follows the symmetric path through the **CompanionToCom** ring buffer.

### Signal Flow

1. External app calls `SetCommState` (or similar) to set baud rate to 115200
2. Windows sends `IOCTL_SERIAL_SET_BAUD_RATE` to the COM device
3. The driver updates `SignalState.BaudRate`, increments the sequence number, sets `ChangedMask |= GCOM_CHANGED_BAUD`
4. The driver completes any pending `IOCTL_GCOM_WAIT_SIGNAL_CHANGE` on the companion device
5. The addon's signal-watcher thread receives the `GcomSignalState` struct
6. It invokes a `ThreadsafeFunction` to deliver the decoded `SignalState` to JS
7. `VirtualPort` emits `"signal"` with the full state snapshot

### Null-Modem Crossover

The driver implements standard null-modem signal crossover:

| Your code sets | External app sees |
|---------------|-------------------|
| `dtr: true` | DSR + DCD asserted |
| `rts: true` | CTS asserted |

| External app sets | Your code sees |
|-------------------|---------------|
| DTR | `signal.dtr === true` |
| RTS | `signal.rts === true` |

## Driver Signing

For development, the driver can be self-signed with test signing enabled. For production distribution:

1. Obtain an EV code signing certificate
2. Run the driver through Microsoft's [Hardware Lab Kit (HLK)](https://learn.microsoft.com/en-us/windows-hardware/test/hlk/)
3. Submit for [attestation signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation) via the Hardware Dev Center

Alternatively, consider a UMDF v2 (User-Mode Driver Framework) port, which only requires standard Authenticode signing.

## License

MIT
