// Steam's live app-details API, with readback and exact rollback ownership.
export function withTimeout(task, timeout = 15000, message = 'GSF: нет ответа от Decky. Закройте и снова откройте панель.') {
  let timer;
  return Promise.race([task, new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(message)), timeout);
  })]).finally(() => clearTimeout(timer));
}

export async function copyCommand(text, clipboard = globalThis.navigator?.clipboard, doc = globalThis.document) {
  try {
    if (clipboard?.writeText) {
      await withTimeout(clipboard.writeText(text), 1500, 'Буфер обмена не отвечает');
      return;
    }
  } catch { /* Some Steam CEF versions deny the asynchronous clipboard API. */ }
  const field = doc?.createElement('textarea');
  if (!field) throw Error('Буфер обмена недоступен. Выделите и скопируйте команду вручную.');
  const focused = doc.activeElement;
  try {
    field.value = text;
    field.style.position = 'fixed';
    field.style.opacity = '0';
    doc.body.appendChild(field);
    field.select();
    if (!doc.execCommand?.('copy')) throw Error('Steam не разрешил копирование. Выделите команду вручную.');
  } finally {field.remove(); focused?.focus?.();}
}

export function readOptions(apps, appid, expected, timeout = 6000) {
  return new Promise((resolve, reject) => {
    let subscription;
    let settled = false;
    const cleanup = () => {
      try {
        if (typeof subscription === 'function') subscription();
        else subscription?.unregister?.();
      } catch { /* A stale CEF subscription must not prevent promise settlement. */ }
    };
    const finish = (error, value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      cleanup();
      error ? reject(error) : resolve(value);
    };
    const timer = setTimeout(() => finish(new Error("Steam не подтвердил параметры запуска. Используйте «Ручной запуск».")), timeout);
    try {
      if (typeof apps?.RegisterForAppDetails !== 'function')
        throw Error('Steam API недоступен. Используйте «Ручной запуск».');
      subscription = apps.RegisterForAppDetails(appid, (details) => {
        const value = details?.strLaunchOptions;
        if (typeof value === "string" && (expected === undefined || value === expected)) finish(null, value);
      });
      if (settled) cleanup();
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
