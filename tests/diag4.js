// Diagnostic 4: direct Win32 CreateFile using Bun FFI + Python fallback
import { createRequire } from 'node:module';
import { spawnSync } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';

const req = createRequire(import.meta.url);
const native = req('../addon/ghostcom.node');

// Clean up
for (const p of native.listPorts()) try { native.destroyPort(p.companionIndex); } catch {}
await sleep(200);

const { portNumber, companionIndex } = native.createPort(0);
console.log(`Port: COM${portNumber}, ci=${companionIndex}`);

const port = native.openPort(companionIndex);
const stream = port.createStream();
const received = [];
stream.onData(chunk => { received.push(chunk); });
stream.onReadError(e => console.error('[read err]', e));
stream.resumeReading();
port.onSignalChange(s => console.log(`[signal] 0x${s.changedMask.toString(16)}`));
await sleep(100);

// Try via Python to get the real Win32 error
const py = spawnSync('C:\\Users\\test\\AppData\\Local\\Microsoft\\WindowsApps\\python.exe', ['-c', `
import ctypes, ctypes.wintypes as wt, sys
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.CreateFileW.restype = wt.HANDLE
k.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE]
INVALID = wt.HANDLE(-1).value
h = k.CreateFileW('\\\\\\\\.\\\\COM${portNumber}', 0xC0000000, 3, None, 3, 0, None)
err = ctypes.get_last_error()
if h == INVALID or h == 0:
    print(f'FAIL:CreateFile error {err}')
    sys.exit(1)
print(f'OPEN:handle={h}')
# Try ReadFile sync
buf = (ctypes.c_char * 64)()
read = wt.DWORD(0)
ok = k.ReadFile(h, buf, 64, ctypes.byref(read), None)
print(f'ReadFile: ok={ok} n={read.value} err={ctypes.get_last_error()}')
if read.value > 0:
    print(f'DATA:{buf.value}')
k.CloseHandle(h)
`]);
console.log('Python stdout:', py.stdout.toString());
if (py.stderr.toString()) console.error('Python stderr:', py.stderr.toString().slice(0, 200));

stream.shutdown(); port.shutdownSignals(); port.close();
native.destroyPort(companionIndex);
