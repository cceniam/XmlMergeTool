import { chromium } from 'playwright';
import { pathToFileURL } from 'node:url';
import path from 'node:path';
import fs from 'node:fs/promises';
import assert from 'node:assert/strict';

const root = path.resolve(process.argv[2] || 'build/cpp/acceptance/features');
const channel = process.env.PLAYWRIGHT_CHANNEL || 'chrome';
// CI installs Playwright's bundled Chromium; local runs keep using Chrome.
const browser = await chromium.launch({ ...(channel === 'chromium' ? {} : { channel }), headless: true });
const outcomes = [];
try {
  for (const variant of ['native', 'reference']) {
    const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
    // The fixture contains illustrative remote image links. Keep acceptance
    // offline and independent of external network timeouts.
    await page.route(/^https?:\/\//, route => route.abort());
    const errors = [];
    const splitRequests = [];
    const requestFailures = [];
    page.on('pageerror', error => errors.push(error.message));
    page.on('request', request => {
      if (/\/log-\d+\.js(?:[?#]|$)/.test(request.url())) splitRequests.push(request.url());
    });
    page.on('requestfailed', request => {
      if (/\/log-\d+\.js(?:[?#]|$)/.test(request.url())) requestFailures.push(request.url());
    });
    const hasSplitFiles = (await fs.readdir(path.join(root, variant))).some(name => /^log-\d+\.js$/.test(name));
    const log = pathToFileURL(path.join(root, variant, 'log.html')).href;
    const report = pathToFileURL(path.join(root, variant, 'report.html')).href;
    await page.goto(log);
    await page.locator('#s1').waitFor();
    assert.equal(await page.locator('#javascript-disabled').isVisible(), false);
    await page.waitForFunction(() => !window.elementsToExpand?.length);
    await page.locator('#s1 > .element-header').hover();
    await page.locator('#s1 > .element-header a.expand').click();
    await page.locator('#s1-t1-k1').waitFor();
    await page.waitForFunction(() => !window.elementsToExpand?.length && !document.querySelector('#s1 .element-header.closed'));
    assert.ok(await page.locator('.keyword').count() > 30);
    assert.equal(await page.evaluate(() => window.injected), undefined);
    const logText = await page.locator('#s1').innerText();
    assert.match(logText, /Control Flow 中文/);
    assert.match(logText, /WHILE/);
    assert.match(logText, /FINALLY/);
    assert.match(logText, /returned/);
    const selector = page.locator('#log-level-selector select');
    await selector.selectOption('INFO');
    assert.equal(await page.locator('.trace-message:visible').count(), 0);
    await selector.selectOption('TRACE');
    assert.ok(await page.locator('.trace-message:visible').count() > 0);
    await page.locator('#errors a[title="Link to details"]').first().click();
    assert.match(new URL(page.url()).hash, /^#s1-t1-k/);
    await page.screenshot({ path: path.join(root, variant, 'log.png'), fullPage: false });
    await page.locator('#report-or-log-link a').click();
    await page.waitForURL(report);
    await page.locator('#statistics-container table').first().waitFor();
    assert.equal(await page.locator('#javascript-disabled').isVisible(), false);
    const statsText = await page.locator('#statistics-container').innerText();
    await page.locator('a[href="#search"]').click();
    await page.waitForFunction(() => window.prevLocationHash === 'search');
    await page.locator('#search-test').fill('Control Flow*');
    await page.getByRole('button', { name: 'Search', exact: true }).click();
    await page.locator('#test-details tbody tr').waitFor();
    assert.equal(await page.locator('#test-details tbody tr').count(), 1);
    await page.locator('#test-details th').first().click();
    await page.screenshot({ path: path.join(root, variant, 'report.png'), fullPage: false });
    await page.locator('#test-details tbody tr').click();
    await page.waitForURL(url => url.pathname.endsWith('/log.html') && url.hash === '#s1-t1');
    await page.locator('#s1-t1').waitFor();
    assert.ok(await page.locator('#s1-t1').isVisible());
    assert.deepEqual(errors, []);
    assert.deepEqual(requestFailures, []);
    if (hasSplitFiles) assert.ok(splitRequests.length > 0, 'split log must load external keyword data');
    outcomes.push({ variant, logText, statsText, browserErrors: errors, splitFilesLoaded: splitRequests.length });
    await page.close();
  }
  assert.equal(outcomes[0].logText, outcomes[1].logText);
  assert.equal(outcomes[0].statsText, outcomes[1].statsText);
  await fs.writeFile(path.join(root, 'browser-results.json'), JSON.stringify({ passed: true, checks: ['offline file://', 'statistics', 'deep expansion', 'split log loading when enabled', 'message order', 'log level selector', 'error anchors', 'search', 'sort', 'report/log navigation', 'no script injection', 'no page errors'], variants: outcomes.map(({ variant, browserErrors, splitFilesLoaded }) => ({ variant, browserErrors, splitFilesLoaded })) }, null, 2), 'utf8');
  console.log('Browser acceptance passed for native and Python reference pages.');
} finally {
  await browser.close();
}
