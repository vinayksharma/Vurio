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

const MediaLocationsSection: React.FC = () => {
  const [directoryPath, setDirectoryPath] = useState('');
  const [message, setMessage] = useState('');
  const [locations, setLocations] = useState<string[]>([]);
  const [selectedLocation, setSelectedLocation] = useState<string | null>(null);
  const [isCollapsed, setIsCollapsed] = useState(false);
  const [isRemapping, setIsRemapping] = useState(false);
  const openStatusPopup = useStatusPopupStore((state) => state.open);

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

  useEffect(() => {
    refreshLocations();
  }, []);

  const getElectronAPI = (): typeof window.electronAPI | null => {
    if (window.electronAPI) return window.electronAPI;
    const vurio = (window as unknown as { vurioElectron?: typeof window.electronAPI }).vurioElectron;
    if (vurio) return vurio;
    const win = window as unknown as { require?: (id: string) => unknown };
    if (typeof win.require === 'function') {
      try {
        const { ipcRenderer } = win.require('electron') as { ipcRenderer: { invoke: (channel: string, arg?: string) => Promise<unknown> } };
        const os = win.require('os') as { homedir: () => string };
        return {
          selectDirectory: (p?: string) => ipcRenderer.invoke('dialog:selectDirectory', p) as Promise<string | null>,
          getHomeDir: () => os.homedir(),
          revealLog: (p: string) => ipcRenderer.invoke('utils:revealLog', p) as Promise<{ ok: boolean; error?: string }>,
          toggleDevTools: () => ipcRenderer.invoke('utils:toggleDevTools') as Promise<void>,
          exportErrorLog: () => ipcRenderer.invoke('utils:exportErrorLog') as Promise<{ ok: boolean; path?: string; error?: string }>,
        } as unknown as NonNullable<typeof window.electronAPI>;
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
    event.stopPropagation();
    const electronAPI = getElectronAPI();
    if (!electronAPI) {
      setMessage('Directory selection is only available in Electron');
      return;
    }

    if (isRemapping) return;
    setIsRemapping(true);

    try {
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
      const result = await changePath(oldPath, newPath);

      let verified = false;
      if (result.status === 'ok' || result.status === 'partial_success') {
        try {
          await new Promise((resolve) => setTimeout(resolve, 500));
          verified = await verifyRemapSuccess(oldPath, newPath);
        } catch {
          /* continue */
        }
      }

      openStatusPopup(result, oldPath, newPath, verified);

      if (result.status === 'ok' || result.status === 'partial_success') {
        remapEventEmitter.emit({ oldPath, newPath, timestamp: Date.now() });
      }

      await refreshLocations();
      if (selectedLocation === oldPath) setSelectedLocation(null);

      if (result.status === 'ok') {
        const filesUpdated = result.files_updated || 0;
        const verificationRate = result.verification_success_rate
          ? `${(result.verification_success_rate * 100).toFixed(1)}%`
          : 'N/A';
        setMessage(`Path remapped successfully! Updated ${filesUpdated} files (verification: ${verificationRate})`);
      } else if (result.status === 'partial_success') {
        const filesUpdated = result.files_updated || 0;
        const filesFailed = result.files_failed || 0;
        setMessage(`Path remapped with partial success. Updated ${filesUpdated} files, ${filesFailed} failed. ${result.error || ''}`);
      } else {
        setMessage(result.error || 'Failed to remap path');
      }
    } catch (error) {
      console.error('Failed to remap path:', error);
      setMessage((error as Error).message || 'Failed to remap path');
    } finally {
      setIsRemapping(false);
    }
  };

  return (
    <section className="panel scan-panel media-locations-section" style={{ position: 'relative' }}>
      {isRemapping && (
        <div
          style={{
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
            cursor: 'wait',
          }}
        >
          <div
            style={{
              padding: '24px 32px',
              background: 'var(--panel)',
              borderRadius: '12px',
              boxShadow: '0 12px 32px rgba(0, 0, 0, 0.2)',
              textAlign: 'center',
              border: '1px solid var(--panel-border)',
            }}
          >
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
        <span>Media Locations</span>
        <button
          className="ghost small"
          onClick={() => setIsCollapsed(!isCollapsed)}
          aria-expanded={!isCollapsed}
          disabled={isRemapping}
        >
          {isCollapsed ? 'Expand' : 'Collapse'}
        </button>
      </h2>
      <p className="helper-text">Register and manage the folders you want the deduplication service to scan.</p>
      <div
        className={`collapsible ${isCollapsed ? '' : 'open'}`}
        style={{ position: 'relative', pointerEvents: isRemapping ? 'none' : 'auto', opacity: isRemapping ? 0.5 : 1 }}
      >
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
  );
};

export default MediaLocationsSection;
