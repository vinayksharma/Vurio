import React, { useState, useEffect } from 'react';
import {
  registerLocation,
  deregisterLocation,
  changePath,
  fetchServerStatus,
  extractRegisteredDirectories,
  verifyRemapSuccess,
} from '../utils/api';
import { useStatusPopupStore } from '../stores';
import { remapEventEmitter } from '../utils/remapEvents';

interface MediaPanelProps {
  isActive: boolean;
}

const MediaPanel: React.FC<MediaPanelProps> = ({ isActive }) => {
  const [directoryPath, setDirectoryPath] = useState('');
  const [message, setMessage] = useState('');
  const [locations, setLocations] = useState<string[]>([]);
  const [selectedLocation, setSelectedLocation] = useState<string | null>(null);
  const [isCollapsed, setIsCollapsed] = useState(false);
  const [isRemapping, setIsRemapping] = useState(false);
  const openStatusPopup = useStatusPopupStore((state) => state.open);

  useEffect(() => {
    if (isActive) {
      refreshLocations();
    }
  }, [isActive]);

  const refreshLocations = async () => {
    try {
      const status = await fetchServerStatus();
      const dirs = extractRegisteredDirectories(status);
      setLocations(dirs);
    } catch (error) {
      console.error('Failed to refresh locations:', error);
      setMessage('Failed to load registered locations');
    }
  };

  const getElectronAPI = (): typeof window.electronAPI | null => {
    if (window.electronAPI) return window.electronAPI;
    const vurio = (window as unknown as { vurioElectron?: typeof window.electronAPI }).vurioElectron;
    if (vurio) return vurio;
    // Fallback when contextIsolation: false and nodeIntegration: true
    if (typeof (window as any).require === 'function') {
      try {
        const { ipcRenderer } = (window as any).require('electron');
        return {
          selectDirectory: (p?: string) => ipcRenderer.invoke('dialog:selectDirectory', p),
          getHomeDir: () => (window as any).require('os').homedir(),
          revealLog: (p: string) => ipcRenderer.invoke('utils:revealLog', p),
          toggleDevTools: () => ipcRenderer.invoke('utils:toggleDevTools'),
          exportErrorLog: () => ipcRenderer.invoke('utils:exportErrorLog'),
        };
      } catch {
        return null;
      }
    }
    return null;
  };

  const browseDirectory = async () => {
    const electronAPI = getElectronAPI();
    if (!electronAPI) {
      const check = (window as { __ELECTRON_API_CHECK__?: boolean }).__ELECTRON_API_CHECK__;
      setMessage(
        check === false
          ? 'Preload did not expose electronAPI. Run from terminal and check console. Rebuild: bun run bun:build'
          : 'Directory selection only available in Electron. If using Electron, open DevTools (View > Toggle DevTools) and check window.electronAPI'
      );
      return;
    }
    try {
      const selected = await electronAPI.selectDirectory();
      if (selected) {
        setDirectoryPath(selected);
        setMessage('');
      }
    } catch (error) {
      console.error('Failed to browse directory:', error);
      setMessage('Failed to open directory dialog');
    }
  };

  const handleRegister = async () => {
    if (!directoryPath.trim()) {
      setMessage('Please select a directory');
      return;
    }
    try {
      await registerLocation(directoryPath);
      setMessage('Location registered successfully');
      setDirectoryPath('');
      await refreshLocations();
    } catch (error) {
      const err = error as Error;
      setMessage(err.message || 'Failed to register location');
    }
  };

  const handleDeregister = async (path: string) => {
    try {
      await deregisterLocation(path);
      setMessage('Location deregistered successfully');
      setSelectedLocation(null);
      await refreshLocations();
    } catch (error) {
      const err = error as Error;
      setMessage(err.message || 'Failed to deregister location');
    }
  };

  const handleDeregisterSelected = async () => {
    if (selectedLocation) {
      await handleDeregister(selectedLocation);
    }
  };

  const handleRemap = async (oldPath: string, event: React.MouseEvent) => {
    event.stopPropagation(); // Prevent selecting the location when clicking Remap
    const electronAPI = getElectronAPI();
    if (!electronAPI) {
      setMessage('Directory selection is only available in Electron');
      return;
    }

    if (isRemapping) {
      return; // Prevent multiple remap operations
    }

    // Disable UI immediately when remap button is clicked
    setIsRemapping(true);

    try {
      // Open directory dialog to select new path
      const newPath = await electronAPI.selectDirectory(oldPath);
      if (!newPath) {
        setIsRemapping(false);
        setMessage('No directory selected');
        return;
      }

      if (newPath === oldPath) {
        setIsRemapping(false);
        setMessage('New path must be different from the old path');
        return;
      }

      setMessage('Remapping path... This may take a moment.');
      
      // Call the change-path API
      const result = await changePath(oldPath, newPath);
      
      // Verify remap success by checking server status
      let verified = false;
      if (result.status === 'ok' || result.status === 'partial_success') {
        try {
          // Wait a brief moment for server to update, then verify
          await new Promise(resolve => setTimeout(resolve, 500));
          verified = await verifyRemapSuccess(oldPath, newPath);
        } catch (verifyError) {
          console.error('Verification check failed:', verifyError);
          // Continue even if verification fails
        }
      }
      
      // Show status popup with results
      openStatusPopup(result, oldPath, newPath, verified);
      
      // Emit remap event to invalidate thumbnail caches
      if (result.status === 'ok' || result.status === 'partial_success') {
        remapEventEmitter.emit({
          oldPath,
          newPath,
          timestamp: Date.now(),
        });
      }
      
      // Refresh locations to show updated path
      await refreshLocations();
      
      // Clear selection since the old path no longer exists
      if (selectedLocation === oldPath) {
        setSelectedLocation(null);
      }
      
      // Update message based on result
      if (result.status === 'ok') {
        const filesUpdated = result.files_updated || 0;
        const verificationRate = result.verification_success_rate 
          ? `${(result.verification_success_rate * 100).toFixed(1)}%` 
          : 'N/A';
        setMessage(
          `Path remapped successfully! Updated ${filesUpdated} files (verification: ${verificationRate})`
        );
      } else if (result.status === 'partial_success') {
        const filesUpdated = result.files_updated || 0;
        const filesFailed = result.files_failed || 0;
        setMessage(
          `Path remapped with partial success. Updated ${filesUpdated} files, ${filesFailed} failed. ${result.error || ''}`
        );
      } else {
        setMessage(result.error || 'Failed to remap path');
      }
    } catch (error) {
      console.error('Failed to remap path:', error);
      const err = error as Error;
      setMessage(err.message || 'Failed to remap path');
    } finally {
      // Re-enable UI after remapping completes
      setIsRemapping(false);
    }
  };

  return (
    <section id="panel-media" className={`content-panel ${isActive ? 'active' : ''}`}>
      <header className="panel-header">
        <div>
          <h1>Media Locations</h1>
          <p className="panel-description">
            Register and manage the folders you want the deduplication service to scan.
          </p>
        </div>
      </header>
      <section className="panel scan-panel" style={{ position: 'relative' }}>
        {isRemapping && (
          <div style={{
            position: 'absolute',
            inset: 0,
            background: 'rgba(255, 255, 255, 0.85)',
            backdropFilter: 'blur(4px)',
            zIndex: 10000,
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            borderRadius: '12px',
            pointerEvents: 'all',
            cursor: 'wait'
          }}>
            <div style={{
              padding: '24px 32px',
              background: 'var(--panel)',
              borderRadius: '12px',
              boxShadow: '0 12px 32px rgba(0, 0, 0, 0.2)',
              textAlign: 'center',
              border: '1px solid var(--panel-border)'
            }}>
              <div style={{ marginBottom: '12px', fontSize: '18px', fontWeight: 600, color: 'var(--text)' }}>
                Remapping in progress...
              </div>
              <div style={{ fontSize: '14px', color: 'var(--muted)' }}>
                Please wait while the path is being remapped.
              </div>
            </div>
          </div>
        )}
        <h2 className="panel-title">
          <span>Register Media Location</span>
          <button
            className="ghost small"
            onClick={() => setIsCollapsed(!isCollapsed)}
            aria-expanded={!isCollapsed}
            disabled={isRemapping}
          >
            {isCollapsed ? 'Expand' : 'Collapse'}
          </button>
        </h2>
        <div className={`collapsible ${isCollapsed ? '' : 'open'}`} style={{ position: 'relative', pointerEvents: isRemapping ? 'none' : 'auto', opacity: isRemapping ? 0.5 : 1 }}>
          <div className="field-row">
            <input
              id="directory-path"
              type="text"
              placeholder="Select a directory..."
              value={directoryPath}
              onChange={(e) => setDirectoryPath(e.target.value)}
              disabled={isRemapping}
            />
            <button id="browse-btn" onClick={browseDirectory} disabled={isRemapping}>
              Browse
            </button>
            <button id="scan-btn" className="primary" onClick={handleRegister} disabled={isRemapping}>
              Register
            </button>
          </div>
          <div id="scan-message" className="message">
            {message}
          </div>
          <div className="targets">
            <h3>Registered Locations</h3>
            <p className="helper-text">Choose a folder below to manage or deregister it.</p>
            <ul id="targets-list">
              {locations.map((location) => (
                <li
                  key={location}
                  className={selectedLocation === location ? 'selected' : ''}
                  onClick={() => !isRemapping && setSelectedLocation(location)}
                  style={{ cursor: isRemapping ? 'not-allowed' : 'pointer' }}
                >
                  <span className="location-path">{location}</span>
                  <button
                    className="remap-btn"
                    onClick={(e) => handleRemap(location, e)}
                    title="Remap this location to a different path"
                    disabled={isRemapping}
                  >
                    Remap
                  </button>
                </li>
              ))}
            </ul>
            <div className="field-row" style={{ marginTop: '12px' }}>
              <button
                id="deregister-selected-btn"
                className="ghost"
                disabled={!selectedLocation || isRemapping}
                onClick={handleDeregisterSelected}
              >
                Deregister Selected
              </button>
            </div>
          </div>
        </div>
      </section>
    </section>
  );
};

export default MediaPanel;
