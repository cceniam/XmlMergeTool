// Smoke-test a real input without expanding the entire potentially huge log.
// Usage: node cpp/tests/browser-sample.mjs ARTIFACT_DIR 'test name pattern'
import { chromium } from 'playwright';
import { pathToFileURL } from 'node:url';
import path from 'node:path';
import fs from 'node:fs/promises';
import assert from 'node:assert/strict';

const root = path.resolve(process.argv[2]);
const pattern = process.argv[3] || '*';
const browser = await chromium.launch({ channel: process.env.PLAYWRIGHT_CHANNEL || 'chrome', headless: true });
const results = [];
try {
  for (const variant of ['native', 'reference']) {
    const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
    page.setDefaultTimeout(60000);
    // Real logs can contain external attachments; acceptance is offline.
    await page.route(/^https?:/, route => route.abort());
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    const report = pathToFileURL(path.join(root, variant, 'report.html')).href;
    await page.goto(report);
    await page.locator('#statistics-container table').first().waitFor();
    const statistics = await page.locator('#statistics-container').innerText();
    await page.locator('a[href="#search"]').click();
    await page.waitForFunction(() => window.prevLocationHash === 'search');
    await page.locator('#search-test').fill(pattern);
    await page.getByRole('button', { name: 'Search', exact: true }).click();
    const rows = page.locator('#test-details tbody tr');
    await rows.first().waitFor();
    await page.locator('#test-details th').first().click();
    const search = await page.locator('#test-details').innerText();
    await rows.first().click();
    await page.waitForURL(url => url.pathname.endsWith('/log.html') && !!url.hash);
    const anchor = new URL(page.url()).hash;
    await page.locator(anchor).waitFor();
    await page.waitForFunction(() => !window.elementsToExpand?.length);
    await page.locator(`${anchor} > .element-header`).hover();
    await page.locator(`${anchor} > .element-header a.expand`).click();
    await page.waitForFunction(id => !window.elementsToExpand?.length &&
      !document.querySelector(`${id} .element-header.closed`), anchor);
    assert.ok(await page.locator(`${anchor} .keyword`).count() > 0);
    const logText = await page.locator(anchor).innerText();
    const level = page.locator('#log-level-selector select');
    if (await level.count()) {
      await level.selectOption('INFO');
      assert.equal(await page.locator(`${anchor} .trace-message:visible`).count(), 0);
      await level.selectOption('TRACE');
    }
    await page.locator('#report-or-log-link a').click();
    await page.waitForURL(report);
    assert.deepEqual(errors, []);
    results.push({ statistics, search, anchor, logText });
    await page.close();
  }
  assert.deepEqual(results[0], results[1]);
  await fs.writeFile(path.join(root, 'browser-sample-results.json'), JSON.stringify({
    passed: true, checks: ['offline file://', 'statistics', 'search', 'sort',
      'test anchor', 'test keyword expansion', 'log levels', 'bidirectional navigation',
      'matching visible content', 'no page errors'],
  }, null, 2));
  console.log('Real-input browser comparison passed.');
} finally {
  await browser.close();
}
