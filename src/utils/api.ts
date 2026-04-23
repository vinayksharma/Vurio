import type { ServerStatus, DuplicateGroupsResponse, Configuration, ChangePathResponse } from '../types';

// Detect Electron: check for electronAPI or if we're in a file:// protocol
// Must explicitly check for electronAPI first, as it's the most reliable indicator
const isElectron = typeof window !== 'undefined' && (
  (typeof (window as any).electronAPI !== 'undefined' && (window as any).electronAPI !== null && !!(window as any).electronAPI) ||
  window.location.protocol === 'file:' ||
  (navigator.userAgent && navigator.userAgent.includes('Electron'))
);

// Force browser mode if we're on http/https and electronAPI is not available
// This ensures we use the Vite proxy in browser mode
const forceBrowserMode = typeof window !== 'undefined' &&
  (window.location.protocol === 'http:' || window.location.protocol === 'https:') &&
  (typeof (window as any).electronAPI === 'undefined' || (window as any).electronAPI === null);

// In Electron, always use localhost:8088 for API, regardless of protocol
// In browser, use empty string (relative URLs) to go through Vite proxy
// Vite proxy is configured in vite.config.ts to proxy /api/* to http://localhost:8088
export const API_BASE = (isElectron && !forceBrowserMode) ? 'http://localhost:8088' : '';

// Debug logging - always log in development
if (typeof window !== 'undefined') {
  const isDev = import.meta.env.DEV || process.env.NODE_ENV === 'development';
  if (isDev) {
    console.log('[API] Environment detection:', {
      isElectron,
      protocol: window.location.protocol,
      userAgent: navigator.userAgent,
      hasElectronAPI: typeof (window as any).electronAPI !== 'undefined' && !!(window as any).electronAPI,
      electronAPIType: typeof (window as any).electronAPI,
      API_BASE: API_BASE || '(empty - using relative URLs)',
      origin: window.location.origin,
    });
  }
}

if (typeof window !== 'undefined') {
  window.API_BASE = API_BASE || window.location.origin;
  // Also set a flag for debugging
  (window as any).__IS_ELECTRON__ = isElectron;
}

export async function safeFetch(path: string, options: RequestInit = {}): Promise<unknown> {
  const headers = { 'Content-Type': 'application/json', ...(options.headers || {}) };
  const fullUrl = `${API_BASE}${path}`;
  try {
    console.log(`[API] Making request to: ${fullUrl}`, {
      isElectron: typeof window !== 'undefined' && (window as any).__IS_ELECTRON__,
      protocol: typeof window !== 'undefined' ? window.location.protocol : 'unknown',
      hasElectronAPI: typeof window !== 'undefined' && !!window.electronAPI,
      API_BASE,
      options,
      headers
    });
    const res = await fetch(fullUrl, { ...options, headers });
    console.log(`[API] Response status: ${res.status} ${res.statusText} for ${fullUrl}`);

    if (!res.ok) {
      let msg = `${res.status} ${res.statusText}`;
      try {
        const j = await res.json();
        msg += ` - ${JSON.stringify(j)}`;
      } catch {
        const text = await res.text();
        msg += ` - ${text}`;
      }
      throw new Error(msg);
    }
    const ct = res.headers.get('content-type') || '';
    return ct.includes('application/json') ? res.json() : res.text();
  } catch (err) {
    console.error('Fetch error:', err);
    throw err;
  }
}

export async function checkOnline(): Promise<boolean> {
  try {
    const response = await fetch(`${API_BASE}/api/v1/server/status`, {
      method: 'GET',
      signal: AbortSignal.timeout(3000),
    });
    if (response.ok) {
      return true;
    }
    return false;
  } catch {
    return false;
  }
}

export async function fetchServerStatus(): Promise<ServerStatus> {
  return safeFetch('/api/v1/server/status') as Promise<ServerStatus>;
}

export async function fetchDuplicateGroups(
  start: number,
  limit: number,
  mode?: string | null
): Promise<DuplicateGroupsResponse> {
  const modeParam = mode ? `&mode=${encodeURIComponent(mode)}` : '';
  return safeFetch(`/api/v1/duplicates/groups?start=${start}&limit=${limit}${modeParam}`) as Promise<DuplicateGroupsResponse>;
}

export async function registerLocation(path: string): Promise<void> {
  await safeFetch('/api/v1/media-locations/register', {
    method: 'POST',
    body: JSON.stringify({ directory: path }),
  });
}

export async function deregisterLocation(path: string): Promise<void> {
  await safeFetch('/api/v1/media-locations/deregister', {
    method: 'POST',
    body: JSON.stringify({ directory: path }),
  });
}

export async function changePath(
  oldPath: string,
  newPath: string,
  sampleSize: number = 20
): Promise<ChangePathResponse> {
  return safeFetch('/api/v1/media-locations/change-path', {
    method: 'POST',
    body: JSON.stringify({
      old_path: oldPath,
      new_path: newPath,
      sample_size: sampleSize,
    }),
  }) as Promise<ChangePathResponse>;
}

export async function verifyRemapSuccess(
  oldPath: string,
  newPath: string
): Promise<boolean> {
  try {
    const status = await fetchServerStatus();
    const registeredDirs = extractRegisteredDirectories(status);
    // Verify that the new path is registered and old path is not
    const hasNewPath = registeredDirs.includes(newPath);
    const hasOldPath = registeredDirs.includes(oldPath);
    return hasNewPath && !hasOldPath;
  } catch (error) {
    console.error('Failed to verify remap success:', error);
    return false;
  }
}

export async function fetchConfiguration(): Promise<Configuration> {
  return safeFetch('/api/v1/config') as Promise<Configuration>;
}

export async function saveConfiguration(config: Configuration): Promise<void> {
  // Save each property individually
  for (const [key, value] of Object.entries(config)) {
    await safeFetch(`/api/v1/config/${encodeURIComponent(key)}`, {
      method: 'PUT',
      body: JSON.stringify({ value }),
    });
  }
}

export async function reloadConfiguration(): Promise<void> {
  await safeFetch('/api/v1/config/reload', { method: 'POST' });
}

export async function resetErrors(): Promise<void> {
  await safeFetch('/api/v1/files/reset-errors', { method: 'POST' });
}

export async function resetDuplicates(): Promise<void> {
  await safeFetch('/api/v1/duplicates/reset', { method: 'DELETE' });
}

export function extractScannedCount(statusObj: ServerStatus): number {
  return statusObj?.scanned_files_count || 0;
}

export function extractProcessedCount(statusObj: ServerStatus): number {
  return statusObj?.processed_files_count || 0;
}

export function extractRegisteredDirectories(statusObj: ServerStatus): string[] {
  return statusObj?.registered_directories || [];
}

export async function saveUserSetting(key: string, value: string): Promise<void> {
  await safeFetch(`/api/v1/user-settings/${encodeURIComponent(key)}`, {
    method: 'PUT',
    body: JSON.stringify({ value }),
  });
}

export async function fetchUserSettings(): Promise<Array<{key: string, value: string}>> {
  try {
    const raw = await safeFetch('/api/v1/user-settings') as Record<string, string>;
    if (!raw || typeof raw !== 'object') return [];
    
    return Object.entries(raw).map(([key, value]) => ({ key, value }));
  } catch {
    return [];
  }
}

export async function fetchUserSetting(key: string): Promise<string | null> {
  try {
    const res = await safeFetch(`/api/v1/user-settings/${encodeURIComponent(key)}`) as { value: string };
    return res.value;
  } catch {
    return null;
  }
}

export async function ejectDuplicates(filePaths: string[]): Promise<{ success: boolean; success_count: number; failure_count: number; error?: string }> {
  return safeFetch('/api/v1/duplicates/eject', {
    method: 'POST',
    body: JSON.stringify({ files: filePaths }),
  }) as Promise<any>;
}

export async function undoEjectDuplicates(filePaths: string[]): Promise<{ success: boolean; success_count: number; failure_count: number; error?: string }> {
  return safeFetch('/api/v1/duplicates/undo-eject', {
    method: 'POST',
    body: JSON.stringify({ files: filePaths }),
  }) as Promise<any>;
}

export async function bulkUndoEjectDuplicates(): Promise<{ success: boolean; success_count: number; failure_count: number; error?: string }> {
  return safeFetch('/api/v1/duplicates/undo-eject', {
    method: 'POST',
    body: JSON.stringify({ undo_all: true }),
  }) as Promise<any>;
}

export async function configurePurityOrder(purityOrderString: string): Promise<{ status: string; message: string; error?: string }> {
  return safeFetch('/api/v1/user-settings/purity-order', {
    method: 'POST',
    body: JSON.stringify({ purity_order: purityOrderString }),
  }) as Promise<any>;
}

export async function ejectErrors(): Promise<{ success: boolean; success_count: number; failure_count: number; error?: string }> {
  return safeFetch('/api/v1/files/eject-errors', {
    method: 'POST',
  }) as Promise<any>;
}

export async function undoEjectErrors(): Promise<{ success: boolean; success_count: number; failure_count: number; error?: string }> {
  return safeFetch('/api/v1/files/undo-eject-errors', {
    method: 'POST',
  }) as Promise<any>;
}

export async function purgeEjectedAndErrored(): Promise<{ success: boolean; success_count: number; failure_count: number; error?: string }> {
  return safeFetch('/api/settings/purge-ejected-errored', {
    method: 'POST',
  }) as Promise<any>;
}
