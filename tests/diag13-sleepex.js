import path from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';

const worker = new Worker(path.resolve(import.meta.dir, './worker-sleepex-test.js'));
const msgs = [];
const done = new Promise(res => {
  worker.onmessage = e => {
    console.log('[worker]', JSON.stringify(e.data));
    msgs.push(e.data);
    if (e.data.type === 'done') res();
  };
  worker.onerror = e => { console.error('[error]', e.message); res(); };
});

await Promise.race([done, sleep(5000)]);
worker.terminate();

const sleepExMsg = msgs.find(m => m.type === 'sleepex_returned');
if (sleepExMsg) {
  console.log(`\nSleepEx returned: ${sleepExMsg.result} (expected 192=APC_FIRED)`);
  if (sleepExMsg.result === 192) {
    console.log('✓ SleepEx correctly enters alertable wait in Bun Worker');
    console.log('  APCs ARE delivered to Bun Worker threads');
    console.log('  → serialport issue is NOT alertable wait blocking');
    console.log('  → issue is uv_async_send from serialport\'s ReadThread not waking Bun event loop');
  } else {
    console.log('✗ SleepEx did NOT return WAIT_IO_COMPLETION');
    console.log(`  Returned ${sleepExMsg.result} instead of 192`);
    console.log('  → Bun Workers do NOT support APC delivery via SleepEx(alertable)');
    console.log('  → This explains why serialport ReadFileEx doesn\'t work in Bun');
  }
} else {
  console.log('✗ Test timed out — worker never called SleepEx');
}
