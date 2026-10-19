// Steam's live app-details API, with readback and exact rollback ownership.
export function readOptions(apps, appid, expected, timeout = 6000) {
  return new Promise((resolve, reject) => {
    let subscription;
    let settled = false;
    const finish = (error, value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      subscription?.unregister();
      error ? reject(error) : resolve(value);
    };
    const timer = setTimeout(() => finish(new Error("Steam не подтвердил параметры запуска")), timeout);
    try {
      subscription = apps.RegisterForAppDetails(appid, (details) => {
        const value = details?.strLaunchOptions;
        if (typeof value === "string" && (expected === undefined || value === expected)) finish(null, value);
      });
      if (settled) subscription?.unregister();
    } catch (error) { finish(error); }
  });
}

export async function enableGame(apps, rpc, appid) {
  const previous = await readOptions(apps, appid);
  const prepared = await rpc.prepare(appid, previous);
  try {
    // Install the listener first: SetAppLaunchOptions has no return value.
    const confirmation = readOptions(apps, appid, prepared.options);
    try { apps.SetAppLaunchOptions(appid, prepared.options); }
    catch (error) { await confirmation.catch(() => {}); throw error; }
    await confirmation;
    await rpc.confirm(appid, prepared.options);
    return prepared.options;
  } catch (error) {
    // Unconfirmed application must never leave active generation armed.
    await rpc.disable(appid, await readOptions(apps, appid).catch(() => previous));
    throw error;
  }
}

export async function disableGame(apps, rpc, appid) {
  const actual = await readOptions(apps, appid);
  const restore = await rpc.disable(appid, actual);
  if (restore.restore) {
    const confirmation = readOptions(apps, appid, restore.options);
    try { apps.SetAppLaunchOptions(appid, restore.options); }
    catch (error) { await confirmation.catch(() => {}); throw error; }
    await confirmation;
    await rpc.forget(appid, restore.options);
  }
  return restore.message;
}

/* Steam Game Mode does not consistently provide navigator.clipboard.
 * Prefer CEF's selection-copy path and fall back only when available.
 * Return failure explicitly: an inert "Copied!" notification is worse than none.
 */
export async function copyLaunchOptions(value, host = globalThis) {
  if (typeof value !== 'string' || !value.includes('%command%')) throw new Error('Invalid Steam launch options');
  const doc = host.document;
  if (doc?.createElement && doc?.body?.appendChild) {
    const node = doc.createElement('textarea');
    node.value = value;
    node.style.position = 'fixed';
    node.style.left = '-9999px';
    doc.body.appendChild(node);
    try {
      node.focus();
      node.select();
      if (doc.execCommand?.('copy') === true) return true;
    } catch { /* fallback below */ }
    finally { doc.body.removeChild(node); }
  }
  if (typeof host.navigator?.clipboard?.writeText === 'function') {
    await host.navigator.clipboard.writeText(value);
    return true;
  }
  throw new Error('Буфер обмена недоступен в Steam Game Mode. Команда показана ниже, используйте ручной ввод.');
}
