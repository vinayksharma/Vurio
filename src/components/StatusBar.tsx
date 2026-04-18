import React from 'react';
import { useServerStatusStore } from '../stores';
import type { ServerStatus } from '../types';

const StatusBar: React.FC = () => {
  const isOnline = useServerStatusStore(state => state.isOnline);
  const scanned = useServerStatusStore(state => state.status?.scanned_files_count || 0);
  const processed = useServerStatusStore(state => state.status?.processed_files_count || 0);
  const threads = useServerStatusStore(state => state.status?.thread_pool?.running_total || 0);
  const maxThreads = useServerStatusStore(state => state.status?.thread_pool?.effective_max_threads || 0);
  const errors = useServerStatusStore(state => state.status?.error_files_count || 0);
  const dupes = useServerStatusStore(state => state.status?.duplicate_groups || 0);
  const dirs = useServerStatusStore(state => state.registeredDirs.length);

  const getStatusSummary = (): string => {
    return `scanned ${scanned} • processed ${processed} • threads ${threads}/${maxThreads} • errors ${errors} • duplicate candidates ${dupes} • registered paths ${dirs}`;
  };

  const getProgressPercent = (): { completed: number; errors: number; errorLeft: number } => {
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

  const progress = getProgressPercent();
  
  const summaryText = getStatusSummary();

  return (
    <footer className="app-statusbar">
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
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
          <span id="status-summary" dangerouslySetInnerHTML={{
            __html: summaryText
              .replace(`errors ${errors}`, errors > 0 ? `<span class="status-error">errors ${errors}</span>` : `errors ${errors}`)
              .replace(`duplicate candidates ${dupes}`, dupes > 0 ? `<span class="status-warning">duplicate candidates ${dupes}</span>` : `duplicate candidates ${dupes}`)
          }}></span>
          <span className={`indicator ${isOnline ? 'online' : 'offline'}`} id="server-indicator" title={isOnline ? 'Server Online' : 'Server Offline'}></span>
        </div>
      </div>
    </footer>
  );
};

export default StatusBar;
