import { create } from 'zustand';
import { fetchServerStatus, checkOnline, API_BASE } from '../utils/api';
import type { ServerStatus } from '../types';

interface ServerStatusState {
  isOnline: boolean;
  status: ServerStatus | null;
  isLoading: boolean;
  scannedCount: number;
  processedCount: number;
  errorCount: number;
  duplicatesFound: number;
  duplicateGroupsCount: number;
  registeredDirs: string[];
  pollStatus: () => Promise<void>;
  startPolling: () => void;
  stopPolling: () => void;
  startRealtimeStatus: () => void;
  stopRealtimeStatus: () => void;
}

let pollingInterval: ReturnType<typeof setInterval> | null = null;
let statusWebSocket: WebSocket | null = null;
let reconnectTimeout: ReturnType<typeof setTimeout> | null = null;

export const useServerStatusStore = create<ServerStatusState>((set, get) => ({
  isOnline: false,
  status: null,
  isLoading: true,
  scannedCount: 0,
  processedCount: 0,
  errorCount: 0,
  duplicatesFound: 0,
  duplicateGroupsCount: 0,
  registeredDirs: [],

  pollStatus: async () => {
    try {
      const online = await checkOnline();
      
      if (online) {
        const serverStatus = await fetchServerStatus();
        set({
          isOnline: true,
          status: serverStatus,
          isLoading: false,
          scannedCount: serverStatus.scanned_files_count || 0,
          processedCount: serverStatus.processed_files_count || 0,
          errorCount: serverStatus.error_files_count || 0,
          duplicatesFound: serverStatus.duplicates_found || 0,
          duplicateGroupsCount: serverStatus.duplicate_groups || 0,
          registeredDirs: serverStatus.registered_directories || [],
        });
      } else {
        set({
          isOnline: false,
          status: null,
          isLoading: false,
          scannedCount: 0,
          processedCount: 0,
          errorCount: 0,
          duplicatesFound: 0,
          duplicateGroupsCount: 0,
          registeredDirs: [],
        });
      }
    } catch (error) {
      console.error('Failed to poll status:', error);
      set({
        isOnline: false,
        status: null,
        isLoading: false,
      });
    }
  },

  startPolling: () => {
    // Try real-time first
    get().startRealtimeStatus();
    
    // Initial poll for immediate data
    get().pollStatus();
    
    // Fallback/Safety polling (less frequent if WebSocket is active)
    if (pollingInterval) {
      clearInterval(pollingInterval);
    }
    pollingInterval = setInterval(() => {
      // Only poll if WebSocket is NOT connected
      if (!statusWebSocket || statusWebSocket.readyState !== WebSocket.OPEN) {
        get().pollStatus();
      }
    }, 5000);
  },

  stopPolling: () => {
    if (pollingInterval) {
      clearInterval(pollingInterval);
      pollingInterval = null;
    }
    get().stopRealtimeStatus();
  },

  startRealtimeStatus: () => {
    if (statusWebSocket) return;

    // Construct WebSocket URL from API_BASE
    let wsUrl: string;
    const wsPath = '/api/v1/server/status/ws';
    
    if (!API_BASE) {
      // Relative URL, use current host
      const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      wsUrl = `${protocol}//${window.location.host}${wsPath}`;
    } else {
      // Absolute URL
      wsUrl = API_BASE.replace(/^http/, 'ws') + wsPath;
    }

    try {
      statusWebSocket = new WebSocket(wsUrl);

      statusWebSocket.onopen = () => {
        console.log('[StatusStore] WebSocket connected');
        if (reconnectTimeout) {
          clearTimeout(reconnectTimeout);
          reconnectTimeout = null;
        }
      };

      statusWebSocket.onmessage = (event) => {
        try {
          const serverStatus: ServerStatus = JSON.parse(event.data);
          set({
            isOnline: true,
            status: serverStatus,
            isLoading: false,
            scannedCount: serverStatus.scanned_files_count || 0,
            processedCount: serverStatus.processed_files_count || 0,
            errorCount: serverStatus.error_files_count || 0,
            duplicatesFound: serverStatus.duplicates_found || 0,
            duplicateGroupsCount: serverStatus.duplicate_groups || 0,
            registeredDirs: serverStatus.registered_directories || [],
          });
        } catch (e) {
          console.error('[StatusStore] Failed to parse WebSocket message:', e);
        }
      };

      statusWebSocket.onclose = () => {
        console.log('[StatusStore] WebSocket closed');
        statusWebSocket = null;
        // Attempt to reconnect after 5 seconds
        if (!reconnectTimeout) {
          reconnectTimeout = setTimeout(() => {
            reconnectTimeout = null;
            get().startRealtimeStatus();
          }, 5000);
        }
      };

      statusWebSocket.onerror = (err) => {
        console.error('[StatusStore] WebSocket error:', err);
        if (statusWebSocket) {
          statusWebSocket.close();
        }
      };
    } catch (e) {
      console.error('[StatusStore] Failed to create WebSocket:', e);
    }
  },

  stopRealtimeStatus: () => {
    if (reconnectTimeout) {
      clearTimeout(reconnectTimeout);
      reconnectTimeout = null;
    }
    if (statusWebSocket) {
      statusWebSocket.close();
      statusWebSocket = null;
    }
  },
}));

if (typeof window !== 'undefined' && window.electronAPI) {
  if (window.electronAPI.onSuspend) {
    window.electronAPI.onSuspend(() => {
      console.log('[StatusStore] System suspended, stopping polling');
      useServerStatusStore.getState().stopPolling();
    });
  }
  if (window.electronAPI.onResume) {
    window.electronAPI.onResume(() => {
      console.log('[StatusStore] System resumed, starting polling');
      useServerStatusStore.getState().startPolling();
    });
  }
}
