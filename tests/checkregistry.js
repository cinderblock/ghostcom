import { createRequire } from 'node:module';
import { spawnSync } from 'node:child_process';

const r = createRequire(import.meta.url);
const n = r('../addon/ghostcom.node');

// Create a port and check registry immediately
const p = n.createPort(0);
console.log('created:', JSON.stringify(p));

// Check registry via PowerShell
const ps = spawnSync('C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe', [
  '-NoProfile', '-Command',
  'Get-ItemProperty "HKLM:\\HARDWARE\\DEVICEMAP\\SERIALCOMM" -ErrorAction SilentlyContinue | ConvertTo-Json'
]);
console.log('SERIALCOMM registry:', ps.stdout.toString() || '(empty)');
if (ps.stderr.toString()) console.error('PS stderr:', ps.stderr.toString());

// Destroy port
n.destroyPort(p.companionIndex);
console.log('destroyed');

// Check registry after destroy
const ps2 = spawnSync('C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe', [
  '-NoProfile', '-Command',
  'Get-ItemProperty "HKLM:\\HARDWARE\\DEVICEMAP\\SERIALCOMM" -ErrorAction SilentlyContinue | ConvertTo-Json'
]);
console.log('SERIALCOMM after destroy:', ps2.stdout.toString() || '(empty)');
