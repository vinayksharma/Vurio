import React, { useState, useEffect } from 'react';
import { resetErrors, resetDuplicates, ejectErrors, undoEjectErrors, ejectIgnored, undoEjectIgnored, purgeEjectedAndErrored, startDatabaseIntegrityCheck, getDatabaseIntegrityCheckStatus, IntegrityCheckStatus, backupDatabase, getLatestBackup, clearAllCaches } from '../utils/api';
import { useUIStore } from '../stores';

const AdvancedSection: React.FC = () => {
  const [isResettingErrors, setIsResettingErrors] = useState(false);
  const [isEjectingErrors, setIsEjectingErrors] = useState(false);
  const [isRestoringErrors, setIsRestoringErrors] = useState(false);
  const [isEjectingIgnored, setIsEjectingIgnored] = useState(false);
  const [isRestoringIgnored, setIsRestoringIgnored] = useState(false);
  const [isResettingDuplicates, setIsResettingDuplicates] = useState(false);
  const [isPurgingEjectedErrored, setIsPurgingEjectedErrored] = useState(false);
  const [isIntegrityCheckRunning, setIsIntegrityCheckRunning] = useState(false);
  const [integrityStatus, setIntegrityStatus] = useState<IntegrityCheckStatus | null>(null);
  const [isBackingUp, setIsBackingUp] = useState(false);
  const [isClearingCaches, setIsClearingCaches] = useState(false);
  const [lastBackupPath, setLastBackupPath] = useState<string | null>(null);
  const [isScrollTestRunning, setIsScrollTestRunning] = useState(false);
  const [isCollapsed, setIsCollapsed] = useState(true);
  const setActivePanel = useUIStore((state) => state.setActivePanel);
  const debugOverlayEnabled = useUIStore((state) => state.debugOverlayEnabled);
  const setDebugOverlayEnabled = useUIStore((state) => state.setDebugOverlayEnabled);

  useEffect(() => {
    let mounted = true;
    getLatestBackup().then(res => {
      if (mounted && res.success && res.backup_path) {
        setLastBackupPath(res.backup_path);
      }
    }).catch(console.error);
    return () => { mounted = false; };
  }, []);

  const isAnyActionRunning = isResettingErrors || isEjectingErrors || isRestoringErrors || isEjectingIgnored || isRestoringIgnored || isResettingDuplicates || isPurgingEjectedErrored || isIntegrityCheckRunning || isScrollTestRunning || isBackingUp || isClearingCaches;

  const handleResetErrors = async () => {
    if (!confirm('Are you sure you want to reset all error files? This will clear the list of files that previously failed processing.')) {
      return;
    }
    setIsResettingErrors(true);
    try {
      await resetErrors();
      alert('Error files reset successfully');
    } catch (error) {
      const err = error as Error;
      alert('Failed to reset errors: ' + err.message);
    } finally {
      setIsResettingErrors(false);
    }
  };

  const handleEjectErrors = async () => {
    if (!confirm('WARNING: Are you sure you want to eject all errored files? They will be moved to a unique \'_errored\' folder.\n\nPlease click \'Cancel\' if you would simply like to try Clearing the Errors and retrying them first.')) {
      return;
    }
    setIsEjectingErrors(true);
    try {
      const res = await ejectErrors();
      if (res.success || res.success_count > 0) {
        alert(`Successfully ejected ${res.success_count} errored files. ${res.failure_count > 0 ? `Failed to eject ${res.failure_count} files.` : ''}`);
      } else {
        alert(`Failed to eject error files. ${res.error || ''}`);
      }
    } catch (error) {
      const err = error as Error;
      alert('Failed to eject errors: ' + err.message);
    } finally {
      setIsEjectingErrors(false);
    }
  };

  const handleRestoreErrors = async () => {
    setIsRestoringErrors(true);
    try {
      const res = await undoEjectErrors();
      if (res.success || res.success_count > 0) {
        alert(`Successfully restored ${res.success_count} errored files. They have now been placed back into the processing queue. ${res.failure_count > 0 ? `Failed to restore ${res.failure_count} files.` : ''}`);
      } else {
        alert(`Failed to restore error files. ${res.error || ''}`);
      }
    } catch (error) {
      const err = error as Error;
      alert('Failed to restore errors: ' + err.message);
    } finally {
      setIsRestoringErrors(false);
    }
  };

  const handleEjectIgnored = async () => {
    if (!confirm('WARNING: Are you sure you want to eject all ignored files? They will be moved to a unique \'_ignored\' folder.\n\nThese are files that were skipped because they did not meet the size or quality requirements.')) {
      return;
    }
    setIsEjectingIgnored(true);
    try {
      const res = await ejectIgnored();
      if (res.success || res.success_count > 0) {
        alert(`Successfully ejected ${res.success_count} ignored files. ${res.failure_count > 0 ? `Failed to eject ${res.failure_count} files.` : ''}`);
      } else {
        alert(`Failed to eject ignored files. ${res.error || ''}`);
      }
    } catch (error) {
      const err = error as Error;
      alert('Failed to eject ignored files: ' + err.message);
    } finally {
      setIsEjectingIgnored(false);
    }
  };

  const handleRestoreIgnored = async () => {
    setIsRestoringIgnored(true);
    try {
      const res = await undoEjectIgnored();
      if (res.success || res.success_count > 0) {
        alert(`Successfully restored ${res.success_count} ignored files. They have now been placed back into the processing queue. ${res.failure_count > 0 ? `Failed to restore ${res.failure_count} files.` : ''}`);
      } else {
        alert(`Failed to restore ignored files. ${res.error || ''}`);
      }
    } catch (error) {
      const err = error as Error;
      alert('Failed to restore ignored files: ' + err.message);
    } finally {
      setIsRestoringIgnored(false);
    }
  };

  const handleResetDuplicates = async () => {
    if (!confirm('Are you sure you want to reset all duplicates? This will remove all duplicate findings across every set and mode.')) {
      return;
    }
    setIsResettingDuplicates(true);
    try {
      await resetDuplicates();
      alert('Duplicates reset successfully');
    } catch (error) {
      const err = error as Error;
      alert('Failed to reset duplicates: ' + err.message);
    } finally {
      setIsResettingDuplicates(false);
    }
  };

  const handlePurgeEjectedErrored = async () => {
    if (!confirm('Are you sure you want to purge all ejected and errored files? This permanently removes them from the database and you will lose the ability to undo their ejection. No source files will be deleted from your disk.')) {
      return;
    }
    setIsPurgingEjectedErrored(true);
    try {
      const res = await purgeEjectedAndErrored();
      if (res.success || res.success_count > 0) {
        alert(`Successfully purged ${res.success_count} files from the database.`);
      } else {
        alert(`Failed to purge files. ${res.error || ''}`);
      }
    } catch (error) {
      const err = error as Error;
      alert('Failed to purge ejected and errored files: ' + err.message);
    } finally {
      setIsPurgingEjectedErrored(false);
    }
  };

  const handleToggleDevTools = () => {
    const invokeToggle = async (): Promise<boolean> => {
      if (window.electronAPI?.toggleDevTools) {
        await window.electronAPI.toggleDevTools();
        return true;
      }
      const win = window as unknown as { require?: (id: string) => { ipcRenderer: { invoke: (channel: string) => Promise<void> } } };
      if (typeof win.require === 'function') {
        try {
          const { ipcRenderer } = win.require('electron');
          await ipcRenderer.invoke('utils:toggleDevTools');
          return true;
        } catch {
          // ignore
        }
      }
      return false;
    };
    invokeToggle().then((success) => {
      if (!success) {
        alert(
          'Developer Tools are only available when the app is run in Electron.\n\n' +
          '• If you opened http://localhost:5173 in a browser: run the app with "npm run dev:electron" or "npm run electron" instead.\n\n' +
          '• If you already launched via Electron: try rebuilding (npm run build) then run "npm run electron" again.'
        );
      }
    }).catch(() => {
      alert('Failed to toggle Developer Tools.');
    });
  };

  const pollIntegrityStatus = async () => {
    try {
      const status = await getDatabaseIntegrityCheckStatus();
      setIntegrityStatus(status);
      if (status.running) {
        setTimeout(pollIntegrityStatus, 1500);
      } else {
        setIsIntegrityCheckRunning(false);
        const { current_phase, files_removed, groups_removed, thumbnails_removed, artifacts_removed, errors_removed } = status;
        if (current_phase.startsWith('Failed')) {
          alert(`Integrity Check Failed: ${current_phase}`);
        } else {
          alert(`Integrity Check Complete!\n\nRemoved:\n- Missing Files: ${files_removed}\n- Orphaned Groups: ${groups_removed}\n- Orphaned Thumbnails: ${thumbnails_removed}\n- Orphaned Artifacts: ${artifacts_removed}\n- Orphaned Errors: ${errors_removed}`);
          window.dispatchEvent(new Event('duplicatesForceRefresh'));
        }
      }
    } catch (error) {
      setIsIntegrityCheckRunning(false);
      alert('Lost connection while checking status.');
    }
  };

  const handleIntegrityCheck = async () => {
    if (!confirm('Are you sure you want to run a full Database Integrity Check?\n\nThis will safely purge records of missing files, orphan groups, and orphan metadata. Background scanning will be temporarily paused.')) {
      return;
    }
    setIsIntegrityCheckRunning(true);
    setIntegrityStatus(null);
    try {
      await startDatabaseIntegrityCheck();
      setTimeout(pollIntegrityStatus, 1500);
    } catch (error) {
      setIsIntegrityCheckRunning(false);
      const err = error as Error;
      alert('Failed to start Integrity Check: ' + err.message);
    }
  };

  const handleBackupDatabase = async () => {
    if (!confirm('Are you sure you want to backup the database?\n\nA copy of the current database will be securely created alongside the original file, even if it is currently in use.')) {
      return;
    }
    setIsBackingUp(true);
    setLastBackupPath(null);
    try {
      const res = await backupDatabase();
      if (res.success) {
        setLastBackupPath(res.backup_path || null);
        alert(`Successfully backed up the database to:\n${res.backup_path}`);
      } else {
        alert(`Failed to backup database. ${res.error || ''}`);
      }
    } catch (error) {
      const err = error as Error;
      alert('Failed to backup database: ' + err.message);
    } finally {
      setIsBackingUp(false);
    }
  };

  const handleShowBackupInFinder = async () => {
    if (!lastBackupPath) return;
    try {
      const win = window as any;
      if (win.electronAPI?.showItemInFolder) {
        await win.electronAPI.showItemInFolder(lastBackupPath);
      } else {
        alert('Electron API not available to open folder.');
      }
    } catch (error) {
      const err = error as Error;
      alert('Failed to open folder: ' + err.message);
    }
  };

  const handleClearCaches = async () => {
    if (!confirm('Are you sure you want to clear all thumbnail and preview caches? This will safely delete all generated images. The system will regenerate them automatically as needed.')) {
      return;
    }
    setIsClearingCaches(true);
    try {
      const res = await clearAllCaches();
      if (res.success) {
        if (window.electronAPI?.clearWebCache) {
          await window.electronAPI.clearWebCache();
        }
        alert('Caches cleared successfully! The application will now reload to flush the UI image cache.');
        window.location.reload();
      } else {
        alert(`Failed to clear caches. ${res.error || ''}`);
      }
    } catch (error) {
      const err = error as Error;
      alert('Failed to clear caches: ' + err.message);
    } finally {
      setIsClearingCaches(false);
    }
  };

  const handleStartScrollTest = async () => {
    if (isScrollTestRunning) {
      if (window.stopScrollTest) {
        window.stopScrollTest();
      }
      setIsScrollTestRunning(false);
      return;
    }

    setActivePanel('duplicates');
    await new Promise((resolve) => setTimeout(resolve, 1000));

    if (window.startScrollTest) {
      setIsScrollTestRunning(true);
      window.startScrollTest().finally(() => {
        setIsScrollTestRunning(false);
      });
    } else {
      alert('Scroll test function not available. Make sure the Duplicates panel is loaded.');
    }
  };

  return (
    <section className="panel advanced-panel">
      <h2 className="panel-title" onClick={() => setIsCollapsed(!isCollapsed)} style={{ cursor: 'pointer' }}>
        <span>Advanced</span>
        <span 
          className="config-group-toggle" 
          style={{ transform: isCollapsed ? 'rotate(-90deg)' : 'none', display: 'inline-block' }}
          aria-expanded={!isCollapsed}
        >
          ▼
        </span>
      </h2>
      <p className="helper-text">Maintenance shortcuts and developer tools for troubleshooting the server.</p>
      <div className={`collapsible ${isCollapsed ? '' : 'open'}`}>
      <div className="action-row">
        <div className="action-details">
          <h3>Manage Error Files</h3>
          <p>Clear the list of files that failed processing to retry them, or manage previously errored files.</p>
        </div>
        <div style={{ display: 'flex', gap: '8px' }}>
          <button
            id="reset-errors-btn"
            className="ghost"
            style={{ width: '120px' }}
            onClick={handleResetErrors}
            disabled={isAnyActionRunning}
          >
            {isResettingErrors ? 'Resetting...' : 'Reset Errors'}
          </button>
          <button
            id="eject-errors-btn"
            className="ghost danger"
            style={{ width: '130px' }}
            onClick={handleEjectErrors}
            disabled={isAnyActionRunning}
          >
            {isEjectingErrors ? 'Ejecting...' : 'Eject Errored'}
          </button>
          <button
            id="restore-errors-btn"
            className="ghost"
            style={{ width: '140px' }}
            onClick={handleRestoreErrors}
            disabled={isAnyActionRunning}
          >
            {isRestoringErrors ? 'Restoring...' : 'Restore Errored'}
          </button>
        </div>
      </div>

      <div className="action-row">
        <div className="action-details">
          <h3>Manage Ignored Files</h3>
          <p>Eject files that were skipped due to size or quality thresholds, or restore previously ejected ignored files back to their original locations.</p>
        </div>
        <div style={{ display: 'flex', gap: '8px' }}>
          <div style={{ width: '120px' }}></div>
          <button
            id="eject-ignored-btn"
            className="ghost danger"
            style={{ width: '130px' }}
            onClick={handleEjectIgnored}
            disabled={isAnyActionRunning}
          >
            {isEjectingIgnored ? 'Ejecting...' : 'Eject Ignored'}
          </button>
          <button
            id="restore-ignored-btn"
            className="ghost"
            style={{ width: '140px' }}
            onClick={handleRestoreIgnored}
            disabled={isAnyActionRunning}
          >
            {isRestoringIgnored ? 'Restoring...' : 'Restore Ignored'}
          </button>
        </div>
      </div>

      <div className="action-row">
        <div className="action-details">
          <h3>Reset Duplicates</h3>
          <p>Remove all duplicate findings across every set and mode. Use this when you need to rescan from scratch.</p>
        </div>
        <button
          id="reset-duplicates-btn"
          className="ghost danger"
          onClick={handleResetDuplicates}
          disabled={isAnyActionRunning}
        >
          {isResettingDuplicates ? 'Resetting...' : 'Reset Duplicates'}
        </button>
      </div>
      <div className="action-row">
        <div className="action-details">
          <h3>Purge Ejected and Errored</h3>
          <p>Permanently remove files in the '_ejected' and '_errored' folders from the database. You will lose the ability to undo their ejection, but no actual source files will be deleted.</p>
        </div>
        <button
          id="purge-ejected-errored-btn"
          className="ghost danger"
          onClick={handlePurgeEjectedErrored}
          disabled={isAnyActionRunning}
        >
          {isPurgingEjectedErrored ? 'Purging...' : 'Purge Ejected & Errored'}
        </button>
      </div>
      <div className="action-row">
        <div className="action-details">
          <h3>Database Integrity Check</h3>
          <p>Scrub the database for physical files that have been deleted, clean up orphaned metadata and invalid duplicate groups. This runs safely in the background.</p>
          {isIntegrityCheckRunning && integrityStatus && (
            <p style={{ color: 'var(--text-secondary)', fontSize: '0.85em', marginTop: '4px' }}>
              Phase: {integrityStatus.current_phase} 
              {integrityStatus.total_files > 0 ? ` (${integrityStatus.progress}%) [${integrityStatus.processed_files}/${integrityStatus.total_files}]` : ''}
            </p>
          )}
        </div>
        <button
          id="db-integrity-btn"
          className="ghost warning"
          onClick={handleIntegrityCheck}
          disabled={isAnyActionRunning}
        >
          {isIntegrityCheckRunning 
            ? (integrityStatus ? `Running... ${integrityStatus.progress}%` : 'Starting...') 
            : 'Run Integrity Check'}
        </button>
      </div>
      <div className="action-row">
        <div className="action-details">
          <h3>Backup Database</h3>
          <p>Create a secure, transactionally consistent snapshot of the current database. This is useful before performing large file operations.</p>
        </div>
        <div style={{ display: 'flex', gap: '8px', width: '180px' }}>
          <button
            id="backup-database-btn"
            className="ghost"
            style={{ flex: 1, padding: '0 8px' }}
            onClick={handleBackupDatabase}
            disabled={isAnyActionRunning}
          >
            {isBackingUp ? 'Backing up...' : 'Backup Database'}
          </button>
          <button
            className="ghost"
            style={{ flex: '0 0 36px', padding: 0, display: 'flex', alignItems: 'center', justifyContent: 'center' }}
            onClick={handleShowBackupInFinder}
            disabled={!lastBackupPath || isAnyActionRunning}
            title={lastBackupPath ? "Open folder containing the latest backup" : "No backups found"}
          >
            <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"></path></svg>
          </button>
        </div>
      </div>
      <div className="action-row">
        <div className="action-details">
          <h3>Clear Caches</h3>
          <p>Delete all generated thumbnails and QHD previews. Useful for forcing the system to regenerate images after testing new generation logic.</p>
        </div>
        <button
          id="clear-caches-btn"
          className="ghost warning"
          onClick={handleClearCaches}
          disabled={isAnyActionRunning}
        >
          {isClearingCaches ? 'Clearing...' : 'Clear All Caches'}
        </button>
      </div>
      <div className="action-row">
        <div className="action-details">
          <h3>Debug Overlay</h3>
          <p>Show a live stats overlay on the Duplicates panel (Total, Cache, Queue, Errors, Memory, etc.). Off by default.</p>
        </div>
        <button
          id="debug-overlay-btn"
          className={debugOverlayEnabled ? 'ghost danger' : 'ghost'}
          onClick={() => setDebugOverlayEnabled(!debugOverlayEnabled)}
        >
          {debugOverlayEnabled ? 'Disable Debug Overlay' : 'Enable Debug Overlay'}
        </button>
      </div>
      <div className="action-row">
        <div className="action-details">
          <h3>Toggle DevTools</h3>
          <p>Open the Electron Developer Tools window to inspect elements, watch network traffic, and debug issues.</p>
        </div>
        <button id="open-devtools-btn" className="ghost" onClick={handleToggleDevTools}>
          Toggle DevTools
        </button>
      </div>
      <div className="action-row">
        <div className="action-details">
          <h3>Scroll Test</h3>
          <p>Run a long-running automated scroll test through all duplicate groups. Scrolls one page at a time, waiting 3 seconds between steps. Monitors for crashes and errors, then generates a comprehensive report at the end. Tests UI and server durability under continuous thumbnail generation.</p>
        </div>
        <button
          id="scroll-test-btn"
          className={isScrollTestRunning ? 'ghost danger' : 'ghost'}
          onClick={handleStartScrollTest}
        >
          {isScrollTestRunning ? 'Stop Test' : 'Start Scroll Test'}
        </button>
      </div>
      </div>
    </section>
  );
};

export default AdvancedSection;
