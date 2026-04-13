// Type definitions for the application

export interface ElectronAPI {
  selectDirectory: (startPath?: string) => Promise<string | null>;
  revealLog: (filePath: string) => Promise<{ ok: boolean; error?: string }>;
  toggleDevTools: () => Promise<void>;
  exportErrorLog: () => Promise<{ ok: boolean; path?: string; error?: string }>;
  openPath: (path: string) => Promise<{ ok: boolean; error?: string }>;
  openViewer: (options: { filePath: string; details: string }) => Promise<{ ok: boolean; error?: string }>;
  onSuspend?: (callback: () => void) => void;
  onResume?: (callback: () => void) => void;
}

declare global {
  interface Window {
    electronAPI?: ElectronAPI;
    API_BASE?: string;
    imagePopup?: {
      open: (options: { filePath: string; details: string }) => void;
      close: () => void;
    };
    thumbnailQueue?: {
      getStats: () => { pending: number; active: number; completed: number };
      getActiveRequests: () => string[];
    };
    startScrollTest?: () => Promise<void>;
    stopScrollTest?: () => void;
    errorLogger?: {
      getErrorCount: () => number;
      exportErrorsAsText: () => string;
      clearErrors: () => void;
      logError: (entry: Omit<import('./index').ErrorLogEntry, 'timestamp'>) => void;
    };
    exportErrors?: () => string;
    getErrorCount?: () => number;
    clearErrors?: () => void;
  }
}

export interface ServerStatus {
  server_mode: string;
  scanned_files_count: number;
  processed_files_count: number;
  error_files_count: number;
  queued_files_count: number;
  duplicates_found: number;
  duplicate_groups: number;
  files_with_duplicates: number;
  registered_directories: string[];
  registered_directories_count: number;
  thread_pool: {
    running_total: number;
    effective_max_threads: number;
    queue_depths: Record<string, number>;
    per_type: Record<string, {
      share: number;
      running: number;
      queued: number;
      queue_depth: number;
    }>;
  };
  configuration: {
    valid: boolean;
    config_file: string;
    property_count: number;
    validation_errors: string[];
  };
  timestamp: number;
}

export interface DuplicateGroupMember {
  file_path: string;
  similarity_score?: number;
  file_size?: number;
  added_at?: string;
  created_date?: string;
  is_representative?: boolean;
  rank?: number;
  is_ejected?: boolean;
}

export interface DuplicateGroup {
  id: number;
  representative: string;
  candidates: string[];
  mode?: string;
  similarity_threshold?: number;
  file_count?: number;
  members?: DuplicateGroupMember[];
}

export interface DuplicateGroupsResponse {
  groups: DuplicateGroup[];
  total?: number;
  total_count?: number;
  start: number;
  limit?: number;
  end?: number;
  returned?: number;
}

export interface Configuration {
  [key: string]: unknown;
}

export interface ConfigProperty {
  key: string;
  value: unknown;
  type: string;
  category: string;
  modified?: boolean;
  error?: string;
}

export type PanelType = 'duplicates' | 'settings';

export interface ChangePathResponse {
  status: 'ok' | 'partial_success' | 'error';
  old_path: string;
  new_path: string;
  files_verified?: number;
  files_verified_success?: number;
  total_files?: number;
  files_updated?: number;
  files_failed?: number;
  verification_success_rate?: number;
  update_details?: {
    scanned_files?: { updated: number; failed: number };
    image_artifacts?: { updated: number; failed: number };
    processing_errors?: { updated: number; failed: number };
    duplicate_groups?: { updated: number; failed: number };
    duplicate_members?: { updated: number; failed: number };
    thumbnail_cache?: { updated: number; failed: number };
  };
  error?: string;
}

export interface ErrorLogEntry {
  timestamp: string;
  type: 'error' | 'unhandledRejection' | 'thumbnail' | 'react';
  message: string;
  source?: string;
  lineno?: number;
  colno?: number;
  stack?: string;
  url?: string;
  context?: Record<string, unknown>;
}

export interface RemapEvent {
  oldPath: string;
  newPath: string;
  timestamp: number;
}

export interface ThumbnailRequest {
  id: string;
  url: string;
  imgElement: HTMLImageElement | null;
  thumbnailDiv: HTMLElement | null;
  callback?: (success: boolean, url: string) => void;
  retryCount: number;
  nextRetryTime: number;
  priority: number;
  groupId?: number;
  memberIndex?: number;
  addedAt: number;
  lastVisibilityCheck?: number;
  fireAndForget?: boolean;
}

export interface RequestCallbacks {
  onLoad?: (url: string) => void;
  onError?: (url: string, error: Error) => void;
}
