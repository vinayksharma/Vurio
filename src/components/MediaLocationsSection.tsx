import React, { useState, useEffect } from 'react';
import {
  registerLocation,
  deregisterLocation,
  changePath,
  verifyRemapSuccess,
} from '../utils/api';
import { useStatusPopupStore, useServerStatusStore } from '../stores';
import { remapEventEmitter } from '../utils/remapEvents';

const MediaLocationsSection: React.FC = () => {
  const [directoryPath, setDirectoryPath] = useState('');
  const [message, setMessage] = useState('');
  const locations = useServerStatusStore((state) => state.registeredDirs);
  const [selectedLocation, setSelectedLocation] = useState<string | null>(null);
  const [isCollapsed, setIsCollapsed] = useState(false);
  const [remappingLocation, setRemappingLocation] = useState<string | null>(null);
  const openStatusPopup = useStatusPopupStore((state) => state.open);

  const refreshLocations = async () => {
    try {
      useServerStatusStore.getState().pollStatus();
    } catch (error) {
      console.error('Failed to trigger status poll:', error);
    }
  };

  useEffect(() => {
    if (locations.length === 0) {
      refreshLocations();
    }
  }, []);

  const getElectronAPI = (): typeof window.electronAPI | null => {
    if (window.electronAPI) return window.electronAPI;
    const vurio = (window as unknown as { vurioElectron?: typeof window.electronAPI }).vurioElectron;
    if (vurio) return vurio;
    const win = window as unknown as { require?: (id: string) => unknown };
    if (typeof win.require === 'function') {
      try {
        const { ipcRenderer } = win.require('electron') as { ipcRenderer: { invoke: (channel: string, arg?: string) => Promise<unknown> } };
        return {
          selectDirectory: (p?: string) => ipcRenderer.invoke('dialog:selectDirectory', p) as Promise<string | null>,
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
      const confirmMessage = "Are you sure you want to deregister this path?\n\nAll duplicate finding and scanning threads will be stopped, and indexing data will be removed. Your source files on disk will NOT be touched. Background tasks will resume in their next scheduled run.";
      if (window.confirm(confirmMessage)) {
        await handleDeregister(selectedLocation);
      }
    }
  };

  const handleRemap = async (oldPath: string, event: React.MouseEvent) => {
    event.stopPropagation();
    const electronAPI = getElectronAPI();
    if (!electronAPI) {
      setMessage('Directory selection is only available in Electron');
      return;
    }

    if (remappingLocation) return;
    setRemappingLocation(oldPath);

    try {
      const newPath = await electronAPI.selectDirectory(oldPath);
      if (!newPath) {
        setRemappingLocation(null);
        setMessage('No directory selected');
        return;
      }

      if (newPath === oldPath) {
        setRemappingLocation(null);
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
      setRemappingLocation(null);
    }
  };

  return (
    <section className="panel scan-panel media-locations-section" style={{ position: 'relative' }}>
      <h2 className="panel-title" onClick={() => !remappingLocation && setIsCollapsed(!isCollapsed)} style={{ cursor: remappingLocation ? 'not-allowed' : 'pointer', opacity: remappingLocation ? 0.6 : 1 }}>
        <span>Media Locations</span>
        <span 
          className="config-group-toggle" 
          style={{ transform: isCollapsed ? 'rotate(-90deg)' : 'none', display: 'inline-block' }}
          aria-expanded={!isCollapsed}
        >
          ▼
        </span>
      </h2>
      <p className="helper-text">Register and manage the folders you want the deduplication service to scan.</p>
      <div
        className={`collapsible ${isCollapsed ? '' : 'open'}`}
      >
        <div className="field-row">
          <input
            id="directory-path"
            type="text"
            placeholder="Select a directory..."
            value={directoryPath}
            onChange={(e) => setDirectoryPath(e.target.value)}
            disabled={!!remappingLocation}
          />
          <button id="browse-btn" onClick={browseDirectory} disabled={!!remappingLocation}>
            Browse
          </button>
          <button id="scan-btn" className="primary" onClick={handleRegister} disabled={!!remappingLocation}>
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
                onClick={() => !remappingLocation && setSelectedLocation(location)}
                style={{ cursor: remappingLocation ? 'not-allowed' : 'pointer' }}
              >
                <span className="location-path">{location}</span>
                <button
                  className="remap-btn"
                  onClick={(e) => handleRemap(location, e)}
                  title="Remap this location to a different path"
                  disabled={!!remappingLocation}
                  style={{ display: 'flex', alignItems: 'center', gap: '6px' }}
                >
                  {remappingLocation === location ? (
                    <>
                      <svg viewBox="0 0 50 50" style={{ width: '14px', height: '14px', animation: 'thumbnail-spin 0.8s linear infinite' }}>
                        <circle cx="25" cy="25" r="20" fill="none" stroke="currentColor" strokeWidth="4" strokeDasharray="31.4 31.4" strokeLinecap="round" />
                      </svg>
                      Remapping...
                    </>
                  ) : (
                    'Remap'
                  )}
                </button>
              </li>
            ))}
          </ul>
          <div className="field-row" style={{ marginTop: '12px' }}>
            <button
              id="deregister-selected-btn"
              className="ghost"
              disabled={!selectedLocation || !!remappingLocation}
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
