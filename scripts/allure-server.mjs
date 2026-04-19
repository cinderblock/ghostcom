// Standalone Bun static server for the Allure report.
// Invoked by both the manual desktop shortcut and the autostart Scheduled Task.
const PORT = 4040;
const ROOT = 'C:/GhostCOM-src/allure-report';

Bun.serve({
  port: PORT,
  hostname: '0.0.0.0',
  fetch(req) {
    const url = new URL(req.url);
    const p = url.pathname === '/' ? '/index.html' : url.pathname;
    return new Response(Bun.file(ROOT + p));
  },
});

console.log(`Allure server up on http://0.0.0.0:${PORT}`);
