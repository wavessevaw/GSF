type Apps = {
  RegisterForAppDetails(id: number, callback: (data: {strLaunchOptions: string}) => void): {unregister(): void};
  SetAppLaunchOptions(id: number, options: string): void;
};
type RPC = {
  prepare(id: number, options: string): Promise<{options: string}>;
  confirm(id: number, actual: string): Promise<unknown>;
  disable(id: number, actual: string): Promise<{options: string; restore: boolean; message?: string}>;
  forget(id: number, actual: string): Promise<unknown>;
};
export function readOptions(apps: Apps, id: number, expected?: string, timeout?: number): Promise<string>;
export function enableGame(apps: Apps | undefined, rpc: RPC, id: number): Promise<string>;
export function disableGame(apps: Apps, rpc: RPC, id: number): Promise<string | undefined>;
export function withTimeout<T>(task: Promise<T>, timeout?: number, message?: string): Promise<T>;
export function copyCommand(text: string): Promise<void>;
