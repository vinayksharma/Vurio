// Type definitions for the application

export interface ElectronAPI {
  selectDirectory: (startPath?: string) => Promise<string | null>;
  getHomeDir: () => string;
  revealLog: (filePath: string) => Promise<{ ok: boolean; error?: string }>;
  toggleDevTools: () => Promise<void>;
  exportErrorLog: () => Promise<{ ok: boolean; path?: string; error?: string }>;
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
    };
    exportErrors?: () => string;
    getErrorCount?: () => number;
    clearErrors?: () => void;
  }
}

export interface ServerStatus {
  server_mode?: string;
  scanned_files_count?: number;
  files_scanned?: number;
  scanned_files?: number;
  scannedCount?: number;
  scanned?: number;
  total_scanned?: number;
  processed_files_count?: number;
  files_processed?: number;
  processed_files?: number;
  processedCount?: number;
  processed?: number;
  total_processed?: number;
  active_threads?: number;
  threads?: number;
  max_threads?: number;
  thread_pool?: {
    running_total?: number;
    effective_max_threads?: number;
    queue_depths?: Record<string, number>;
  };
  error_files_count?: number;
  errors?: number;
  error_count?: number;
  duplicate_groups_count?: number;
  duplicate_groups?: number;
  duplicates?: number;
  duplicate_count?: number;
  duplicates_found?: number; // Total files in groups (changes when members added/removed)
  registered_directories?: string[] | Array<{ path: string }>;
  registered_locations?: string[] | Array<{ path: string }>;
  directories?: string[] | Array<{ path: string }>;
  locations?: string[] | Array<{ path: string }>;
  media_locations?: string[] | Array<{ path: string }>;
}

export interface DuplicateGroupMember {
  file_path: string;
  similarity_score?: number;
  file_size?: number;
  added_at?: string;
  is_representative?: boolean;
  rank?: number;
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
