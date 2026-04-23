import React, { useState } from 'react';
import { resetErrors, resetDuplicates, ejectErrors, undoEjectErrors, purgeEjectedAndErrored } from '../utils/api';
import { useUIStore } from '../stores';

const AdvancedSection: React.FC = () => {
  const [isResettingErrors, setIsResettingErrors] = useState(false);
  const [isEjectingErrors, setIsEjectingErrors] = useState(false);
  const [isRestoringErrors, setIsRestoringErrors] = useState(false);
  const [isResettingDuplicates, setIsResettingDuplicates] = useState(false);
  const [isPurgingEjectedErrored, setIsPurgingEjectedErrored] = useState(false);
  const [isScrollTestRunning, setIsScrollTestRunning] = useState(false);
  const [isCollapsed, setIsCollapsed] = useState(false);
  const setActivePanel = useUIStore((state) => state.setActivePanel);
  const debugOverlayEnabled = useUIStore((state) => state.debugOverlayEnabled);
  const setDebugOverlayEnabled = useUIStore((state) => state.setDebugOverlayEnabled);

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
            onClick={handleResetErrors}
            disabled={isResettingErrors || isEjectingErrors || isRestoringErrors}
          >
            {isResettingErrors ? 'Resetting...' : 'Reset Errors'}
          </button>
          <button
            id="eject-errors-btn"
            className="ghost danger"
            onClick={handleEjectErrors}
            disabled={isResettingErrors || isEjectingErrors || isRestoringErrors}
          >
            {isEjectingErrors ? 'Ejecting...' : 'Eject Errored'}
          </button>
          <button
            id="restore-errors-btn"
            className="ghost"
            onClick={handleRestoreErrors}
            disabled={isResettingErrors || isEjectingErrors || isRestoringErrors}
          >
            {isRestoringErrors ? 'Restoring...' : 'Restore Errored'}
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
          disabled={isResettingDuplicates || isPurgingEjectedErrored}
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
          disabled={isResettingDuplicates || isPurgingEjectedErrored}
        >
          {isPurgingEjectedErrored ? 'Purging...' : 'Purge Ejected & Errored'}
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
