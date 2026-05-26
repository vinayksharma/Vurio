import React, { useState, useEffect } from 'react';
import {
  registerLocation,
  deregisterLocation,
  changePath,
  verifyRemapSuccess,
  fetchUserSettings,
} from '../utils/api';
import { useStatusPopupStore, useServerStatusStore } from '../stores';
import { remapEventEmitter } from '../utils/remapEvents';

const MediaLocationsSection: React.FC = () => {
  const [directoryPath, setDirectoryPath] = useState('');
  const [message, setMessage] = useState('');
  const locations = useServerStatusStore((state) => state.registeredDirs);
  const [selectedLocation, setSelectedLocation] = useState<string | null>(null);
  const [isCollapsed, setIsCollapsed] = useState(true);
  const [remappingLocation, setRemappingLocation] = useState<string | null>(null);
  const [deregisteringPath, setDeregisteringPath] = useState<string | null>(null);
  const [ejectionPaths, setEjectionPaths] = useState<Record<string, string[]>>({});
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
    
    fetchUserSettings().then(settings => {
      const pathsToLocationKeys: Record<string, string> = {};
      const locKeyToPaths: Record<string, string[]> = {};

      settings.forEach(s => {
        if (s.key.startsWith('mediaLocation:')) {
          const locKey = s.key.substring('mediaLocation:'.length);
          pathsToLocationKeys[s.value] = locKey;
        } else if (s.key.startsWith('ejectionPath:') || 
                   s.key.startsWith('ejectionPathErrored:') || 
                   s.key.startsWith('ejectionPathIgnored:')) {
          const prefix = s.key.split(':')[0];
          const locKey = s.key.substring(prefix.length + 1);
          if (!locKeyToPaths[locKey]) locKeyToPaths[locKey] = [];
          locKeyToPaths[locKey].push(s.value);
        }
      });
      
      const ejectionPathsMap: Record<string, string[]> = {};
      Object.keys(pathsToLocationKeys).forEach(location => {
        const locKey = pathsToLocationKeys[location];
        if (locKeyToPaths[locKey] && locKeyToPaths[locKey].length > 0) {
          ejectionPathsMap[location] = locKeyToPaths[locKey];
        }
      });
      setEjectionPaths(ejectionPathsMap);
    }).catch(console.error);
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
      setDeregisteringPath(path);
      setMessage('Deregistering location and wiping database gracefully. This may take several seconds, please do not click away...');
      await deregisterLocation(path);
      setMessage('Location deregistered successfully');
      window.dispatchEvent(new Event('duplicatesForceRefresh'));
      setSelectedLocation(null);
      await refreshLocations();
    } catch (error) {
      const err = error as Error;
      setMessage(err.message || 'Failed to deregister location');
    } finally {
      setDeregisteringPath(null);
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
    <section 
      className="panel settings-panel scan-panel media-locations-section" 
      style={{ 
        position: 'relative',
        cursor: (remappingLocation || deregisteringPath) ? 'wait' : (isCollapsed ? 'pointer' : 'default')
      }}
      onClick={() => {
        if (isCollapsed && !remappingLocation && !deregisteringPath) {
          setIsCollapsed(false);
        }
      }}
    >
      <h2 className="panel-title" onClick={(e) => {
        if (!remappingLocation && !deregisteringPath) {
          setIsCollapsed(!isCollapsed);
          e.stopPropagation();
        }
      }} style={{ cursor: 'pointer', opacity: (remappingLocation || deregisteringPath) ? 0.6 : 1 }}>
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
            disabled={!!remappingLocation || !!deregisteringPath}
          />
          <button id="browse-btn" onClick={browseDirectory} disabled={!!remappingLocation || !!deregisteringPath} title="Browse and select a folder to register" style={{ minWidth: '150px', justifyContent: 'center' }}>
            Browse
          </button>
          <button id="scan-btn" className="primary" onClick={handleRegister} disabled={!!remappingLocation || !!deregisteringPath} title="Register the selected folder for deduplication scanning" style={{ minWidth: '150px', justifyContent: 'center' }}>
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
                onClick={() => !remappingLocation && !deregisteringPath && setSelectedLocation(location)}
                style={{ cursor: (remappingLocation || deregisteringPath) ? 'wait' : 'pointer' }}
              >
                <span className="location-path">{location}</span>
                <button
                  className="remap-btn"
                  onClick={(e) => handleRemap(location, e)}
                  title="Remap this location to a different path"
                  disabled={!!remappingLocation || !!deregisteringPath}
                  style={{ display: 'flex', alignItems: 'center', gap: '6px', minWidth: '150px', justifyContent: 'center' }}
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
                <button
                  className="secondary icon-button remap-btn"
                  onClick={(e) => {
                    e.stopPropagation();
                    const paths = ejectionPaths[location];
                    if (paths && paths.length > 0 && window.electronAPI?.openPath) {
                      Promise.all(paths.map(p => window.electronAPI!.openPath(p))).then(results => {
                        const anyOk = results.some(res => res.ok);
                        if (!anyOk) alert('No ejected, errored, or ignored folders found for this location.');
                      });
                    } else if (!window.electronAPI?.openPath) {
                      alert('System path launching is only available in the desktop app.');
                    }
                  }}
                  title="Open Ejected Folder in OS"
                  disabled={!ejectionPaths[location] || ejectionPaths[location].length === 0 || !!remappingLocation || !!deregisteringPath}
                  style={{ display: 'flex', alignItems: 'center', gap: '6px', marginLeft: '8px', minWidth: '150px', justifyContent: 'center' }}
                >
                  <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"></path></svg>
                  Ejected
                </button>
              </li>
            ))}
          </ul>
          <div className="field-row" style={{ marginTop: '12px' }}>
            <button
              id="deregister-selected-btn"
              className="ghost"
              disabled={!selectedLocation || !!remappingLocation || !!deregisteringPath}
              onClick={handleDeregisterSelected}
              title="Deregister and stop scanning the selected location"
              style={{ display: 'flex', alignItems: 'center', gap: '6px', minWidth: '150px', justifyContent: 'center' }}
            >
              {deregisteringPath ? (
                <>
                  <svg viewBox="0 0 50 50" style={{ width: '14px', height: '14px', animation: 'thumbnail-spin 0.8s linear infinite' }}>
                    <circle cx="25" cy="25" r="20" fill="none" stroke="currentColor" strokeWidth="4" strokeDasharray="31.4 31.4" strokeLinecap="round" />
                  </svg>
                  Deregistering...
                </>
              ) : (
                'Deregister Selected'
              )}
            </button>
          </div>
        </div>
      </div>

      {deregisteringPath && (
        <div className="modal-overlay" style={{ position: 'fixed', top: 0, left: 0, right: 0, bottom: 0, background: 'rgba(0,0,0,0.6)', zIndex: 9999, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center' }}>
          <div style={{ background: 'var(--bg-panel, #252526)', padding: '24px', borderRadius: '8px', display: 'flex', flexDirection: 'column', alignItems: 'center', gap: '16px', boxShadow: '0 4px 12px rgba(0,0,0,0.5)' }}>
            <svg viewBox="0 0 50 50" style={{ width: '40px', height: '40px', animation: 'thumbnail-spin 0.8s linear infinite', color: 'var(--accent, #007acc)' }}>
              <circle cx="25" cy="25" r="20" fill="none" stroke="currentColor" strokeWidth="4" strokeDasharray="31.4 31.4" strokeLinecap="round" />
            </svg>
            <h3 style={{ margin: 0, color: 'var(--text-normal, #cccccc)' }}>Deregistering Location</h3>
            <p style={{ margin: 0, color: 'var(--text-muted, #888888)', textAlign: 'center', maxWidth: '350px', lineHeight: '1.4' }}>
              Wiping database indexes gracefully and stopping background tasks. This may take several seconds...
            </p>
          </div>
        </div>
      )}
    </section>
  );
};

export default MediaLocationsSection;
