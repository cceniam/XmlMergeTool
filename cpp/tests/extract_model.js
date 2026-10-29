// Evaluate only generated data declarations, never input log HTML or the UI libraries.
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const html = fs.readFileSync(process.argv[2], 'utf8');
const context = { window: {} };
vm.createContext(context);
let started = false;
for (const match of html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/gi)) {
  const script = match[1];
  if (/window\.output\s*=\s*\{\}/.test(script)) started = true;
  if (!started) continue;
  vm.runInContext(script, context, { timeout: 10000 });
  if (/window\.settings\s*=/.test(script)) break;
}
if (!context.window.output || !context.window.settings) throw new Error('Missing report data');
// Resolve split bodies into the common data model. Each sidecar owns a string
// table, while status timestamps continue to use the main page's baseMillis.
const notifications = [];
context.window.fileLoading = { notify: name => notifications.push(name) };
function remapItem(item, offset) {
  if (item.length === 3) { item[2] += offset; return; }
  for (let i = 1; i < 8; i++) item[i] += offset;
  if (item[8].length > 3) item[8][3] += offset;
  item[9].forEach(child => remapItem(child, offset));
}
function resolveBody(value) {
  if (typeof value !== 'number') return value;
  const filename = `${context.window.settings.splitLogBase}-${value}.js`;
  const script = fs.readFileSync(path.join(path.dirname(process.argv[2]), filename), 'utf8');
  vm.runInContext(script, context, { timeout: 10000 });
  if (!notifications.includes(filename)) throw new Error(`Missing split-log notification: ${filename}`);
  const offset = context.window.output.strings.length;
  context.window.output.strings = context.window.output.strings.concat(context.window[`strings${value}`]);
  const items = context.window[`keywords${value}`];
  items.forEach(item => remapItem(item, offset));
  return items;
}
function resolveSuite(suite) {
  suite[6].forEach(resolveSuite);
  suite[7].forEach(test => { test[5] = resolveBody(test[5]); });
  suite[8].forEach(keyword => { keyword[9] = resolveBody(keyword[9]); });
}
resolveSuite(context.window.output.suite);
process.stdout.write(JSON.stringify(context.window), 'utf8');
