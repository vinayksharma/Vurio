import React from 'react';
import { useServerStatusStore } from '../stores';
import type { ServerStatus } from '../types';

const StatusBar: React.FC = () => {
  const { isOnline, status, registeredDirs } = useServerStatusStore();

  const getStatusSummary = (status: ServerStatus | null): string => {
    if (!status) return 'Scanned 0 • Processed 0 • Threads 0/0 • Errors 0 • Dupes 0 • Dirs 0';
    
    const scanned = status.scanned_files_count || 0;
    const processed = status.processed_files_count || 0;
    const threads = status.thread_pool?.running_total || 0;
    const maxThreads = status.thread_pool?.effective_max_threads || 0;
    const errors = status.error_files_count || 0;
    const dupes = status.duplicate_groups || 0;
    const dirs = registeredDirs.length;

    return `Scanned ${scanned} • Processed ${processed} • Threads ${threads}/${maxThreads} • Errors ${errors} • Dupes ${dupes} • Dirs ${dirs}`;
  };

  const getProgressPercent = (status: ServerStatus | null): { completed: number; errors: number; errorLeft: number } => {
    if (!status) return { completed: 0, errors: 0, errorLeft: 0 };
    
    const scanned = status.scanned_files_count || 0;
    const processed = status.processed_files_count || 0;
    const errors = status.error_files_count || 0;
    
    if (scanned === 0 && errors === 0 && processed === 0) {
      return { completed: 0, errors: 0, errorLeft: 0 };
    }
    
    const total = scanned || 1;
    const completedPercent = Math.round((processed / total) * 100);
    const errorsPercent = Math.round((errors / total) * 100);
    
    return {
      completed: Math.min(completedPercent, 100),
      errors: Math.min(errorsPercent, 100),
      errorLeft: Math.min(completedPercent, 100),
    };
  };

  const progress = getProgressPercent(status);
  const timestamp = new Date().toLocaleTimeString();
  
  const summaryText = getStatusSummary(status);
  const errors = status?.error_files_count || 0;
  const dupes = status?.duplicate_groups || 0;

  return (
    <footer className="app-statusbar">
      <div className="status-left">
        <span className={`indicator ${isOnline ? 'online' : 'offline'}`} id="server-indicator"></span>
        <div className="status-text">
          <span id="server-status-text">{isOnline ? 'Online' : 'Offline'}</span>
          <span id="server-name" className="status-subtitle">Media Deduplication Server</span>
        </div>
      </div>
      <div className="status-center">
        <div className="status-progress">
          <div
            id="statusbar-progress-completed"
            className="status-progress-bar-completed"
            style={{ width: `${progress.completed}%` }}
          ></div>
          <div
            id="statusbar-progress-errors"
            className="status-progress-bar-errors"
            style={{ width: `${progress.errors}%`, left: `${progress.errorLeft}%` }}
          ></div>
        </div>
        <span id="status-summary" dangerouslySetInnerHTML={{
          __html: summaryText
            .replace(`Errors ${errors}`, errors > 0 ? `<span class="status-error">Errors ${errors}</span>` : `Errors ${errors}`)
            .replace(`Dupes ${dupes}`, dupes > 0 ? `<span class="status-warning">Dupes ${dupes}</span>` : `Dupes ${dupes}`)
        }}></span>
      </div>
      <div className="status-right">
        <span id="server-timestamp" className="timestamp">
          {timestamp}
        </span>
      </div>
    </footer>
  );
};

export default StatusBar;
