import { create } from 'zustand';
import { fetchServerStatus, checkOnline, extractScannedCount, extractProcessedCount, extractRegisteredDirectories } from '../utils/api';
import type { ServerStatus } from '../types';

interface ServerStatusState {
  isOnline: boolean;
  status: ServerStatus | null;
  isLoading: boolean;
  scannedCount: number;
  processedCount: number;
  registeredDirs: string[];
  pollStatus: () => Promise<void>;
  startPolling: () => void;
  stopPolling: () => void;
}

let pollingInterval: ReturnType<typeof setInterval> | null = null;

export const useServerStatusStore = create<ServerStatusState>((set, get) => ({
  isOnline: false,
  status: null,
  isLoading: true,
  scannedCount: 0,
  processedCount: 0,
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
          scannedCount: extractScannedCount(serverStatus),
          processedCount: extractProcessedCount(serverStatus),
          registeredDirs: extractRegisteredDirectories(serverStatus),
        });
      } else {
        set({
          isOnline: false,
          status: null,
          isLoading: false,
          scannedCount: 0,
          processedCount: 0,
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
    // Initial poll
    get().pollStatus();
    
    // Set up interval
    if (pollingInterval) {
      clearInterval(pollingInterval);
    }
    pollingInterval = setInterval(() => {
      get().pollStatus();
    }, 3000);
  },

  stopPolling: () => {
    if (pollingInterval) {
      clearInterval(pollingInterval);
      pollingInterval = null;
    }
  },
}));
