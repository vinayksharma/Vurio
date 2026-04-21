import React, { useState, useEffect } from 'react';
import { fetchConfiguration, saveConfiguration, reloadConfiguration } from '../utils/api';
import type { Configuration } from '../types';
import MediaLocationsSection from './MediaLocationsSection';
import UISettingsSection from './UISettingsSection';
import AdvancedSection from './AdvancedSection';
import PurityOrderSection from './PurityOrderSection';

interface SettingsPanelProps {
  isActive: boolean;
}

const SettingsPanel: React.FC<SettingsPanelProps> = ({ isActive }) => {
  const [config, setConfig] = useState<Configuration | null>(null);
  const [isLoading, setIsLoading] = useState(true);
  const [modifiedProperties, setModifiedProperties] = useState<Set<string>>(new Set());
  const [validationErrors, setValidationErrors] = useState<Map<string, string>>(new Map());
  const [collapsedGroups, setCollapsedGroups] = useState<Set<string>>(new Set());
  const [configSectionCollapsed, setConfigSectionCollapsed] = useState(false);

  useEffect(() => {
    if (isActive) {
      loadConfig();
    }
  }, [isActive]);

  const loadConfig = async () => {
    setIsLoading(true);
    try {
      const data = await fetchConfiguration();
      setConfig(data);
      setModifiedProperties(new Set());
      setValidationErrors(new Map());
    } catch (error) {
      console.error('Failed to load configuration:', error);
    } finally {
      setIsLoading(false);
    }
  };

  const handlePropertyChange = (key: string, value: unknown) => {
    if (!config) return;
    
    const originalValue = config[key];
    const stringValue = String(value);
    
    if (stringValue !== String(originalValue)) {
      setModifiedProperties((prev) => new Set(prev).add(key));
      setValidationErrors((prev) => {
        const next = new Map(prev);
        next.delete(key);
        return next;
      });
    } else {
      setModifiedProperties((prev) => {
        const next = new Set(prev);
        next.delete(key);
        return next;
      });
    }
  };

  const validateProperty = (key: string, value: unknown): boolean => {
    if (!config) return true;
    
    const type = getPropertyType(config[key]);
    if (type === 'number') {
      const numValue = Number(value);
      if (isNaN(numValue)) {
        setValidationErrors((prev) => new Map(prev).set(key, 'Must be a valid number'));
        return false;
      }
    } else if (type === 'path' && String(value).trim() === '') {
      setValidationErrors((prev) => new Map(prev).set(key, 'Path cannot be empty'));
      return false;
    }
    
    setValidationErrors((prev) => {
      const next = new Map(prev);
      next.delete(key);
      return next;
    });
    return true;
  };

  const getPropertyType = (value: unknown): string => {
    if (typeof value === 'boolean') return 'boolean';
    if (typeof value === 'number') return 'number';
    if (typeof value === 'string') {
      if (value.includes('\\') || value.includes('/')) return 'path';
      return 'string';
    }
    return 'unknown';
  };

  const getCategoryFromKey = (key: string): string => {
    if (key.startsWith('server.')) return 'Server Configuration';
    if (key.startsWith('database.')) return 'Database Configuration';
    if (key.startsWith('cache.')) return 'Cache Configuration';
    if (key.startsWith('logging.')) return 'Logging Configuration';
    if (key.startsWith('tpm.')) return 'Thread Pool Manager (TPM) Configuration';
    if (key.startsWith('scheduler.')) return 'Scheduler Configuration';
    if (key.startsWith('files.')) return 'Files Manager Configuration';
    if (key.startsWith('media.processor.')) return 'Media Processor Configuration';
    if (key.startsWith('media.image.')) return 'Media Image Processing Configuration';
    if (key.startsWith('media.images.') && key.endsWith('.transcode')) {
      return 'Image Transcode Toggles - Transcode to JPEG before embedding';
    }
    if (key.startsWith('media.images.') && !key.startsWith('media.images.raw.')) {
      return 'Media Format Toggles - Standard Image Formats';
    }
    if (key.startsWith('media.images.raw.')) return 'Media Format Toggles - RAW Image Formats';
    if (key.startsWith('media.audio.')) return 'Media Format Toggles - Audio Formats';
    if (key.startsWith('media.video.')) return 'Media Format Toggles - Video Formats';
    if (key.startsWith('thumbnail.')) return 'Thumbnail Configuration';
    if (key.startsWith('assetPreview.')) return 'Asset Preview Configuration';
    if (key.startsWith('duplicates.')) return 'Duplicate Finder Configuration';
    return 'Other';
  };

  const groupPropertiesByCategory = (config: Configuration): Record<string, Array<{ key: string; value: unknown }>> => {
    const groups: Record<string, Array<{ key: string; value: unknown }>> = {};
    
    for (const [key, value] of Object.entries(config)) {
      const category = getCategoryFromKey(key);
      if (!groups[category]) {
        groups[category] = [];
      }
      groups[category].push({ key, value });
    }
    
    for (const category in groups) {
      groups[category].sort((a, b) => a.key.localeCompare(b.key));
    }
    
    return groups;
  };

  const handleSave = async () => {
    if (!config) return;
    
    const changes: Configuration = {};
    modifiedProperties.forEach((key) => {
      changes[key] = config[key];
    });
    
    try {
      await saveConfiguration(changes);
      await loadConfig();
    } catch (error) {
      const err = error as Error;
      alert('Failed to save configuration: ' + err.message);
    }
  };

  const handleReload = async () => {
    try {
      await reloadConfiguration();
      await loadConfig();
    } catch (error) {
      const err = error as Error;
      alert('Failed to reload configuration: ' + err.message);
    }
  };

  const toggleGroup = (category: string) => {
    setCollapsedGroups((prev) => {
      const next = new Set(prev);
      if (next.has(category)) {
        next.delete(category);
      } else {
        next.add(category);
      }
      return next;
    });
  };

  if (isLoading) {
    return (
      <section id="panel-settings" className={`content-panel ${isActive ? 'active' : ''}`}>
        <header className="panel-header">
          <div>
            <h1>Settings</h1>
            <p className="panel-description">Adjust server configuration and manage how VURIO behaves.</p>
          </div>
        </header>
        <MediaLocationsSection />
        <UISettingsSection />
        <PurityOrderSection />
        <section className="panel settings-panel">
          <h2 className="panel-title" onClick={() => setConfigSectionCollapsed(!configSectionCollapsed)} style={{ cursor: 'pointer' }}>
            <span>Configuration</span>
            <span 
              className="config-group-toggle" 
              style={{ transform: configSectionCollapsed ? 'rotate(-90deg)' : 'none', display: 'inline-block' }}
              aria-expanded={!configSectionCollapsed}
            >
              ▼
            </span>
          </h2>
          <div className={`collapsible ${configSectionCollapsed ? '' : 'open'}`}>
            <div className="config-loading">Loading configuration...</div>
          </div>
        </section>
        <AdvancedSection />
      </section>
    );
  }

  if (!config) {
    return (
      <section id="panel-settings" className={`content-panel ${isActive ? 'active' : ''}`}>
        <header className="panel-header">
          <div>
            <h1>Settings</h1>
            <p className="panel-description">Adjust server configuration and manage how VURIO behaves.</p>
          </div>
        </header>
        <MediaLocationsSection />
        <UISettingsSection />
        <PurityOrderSection />
        <section className="panel settings-panel">
          <h2 className="panel-title" onClick={() => setConfigSectionCollapsed(!configSectionCollapsed)} style={{ cursor: 'pointer' }}>
            <span>Configuration</span>
            <span 
              className="config-group-toggle" 
              style={{ transform: configSectionCollapsed ? 'rotate(-90deg)' : 'none', display: 'inline-block' }}
              aria-expanded={!configSectionCollapsed}
            >
              ▼
            </span>
          </h2>
          <div className={`collapsible ${configSectionCollapsed ? '' : 'open'}`}>
            <div className="config-loading">Failed to load configuration</div>
          </div>
        </section>
        <AdvancedSection />
      </section>
    );
  }

  const groups = groupPropertiesByCategory(config);
  const hasModifications = modifiedProperties.size > 0;
  const hasErrors = validationErrors.size > 0;

  return (
    <section id="panel-settings" className={`content-panel ${isActive ? 'active' : ''}`}>
      <header className="panel-header">
        <div>
          <h1>Settings</h1>
          <p className="panel-description">Adjust server configuration and manage how Dedup Desktop behaves.</p>
        </div>
      </header>
      <MediaLocationsSection />
      <UISettingsSection />
      <section className="panel settings-panel">
        <h2 className="panel-title" onClick={() => setConfigSectionCollapsed(!configSectionCollapsed)} style={{ cursor: 'pointer' }}>
          <span>Configuration</span>
          <span 
            className="config-group-toggle" 
            style={{ transform: configSectionCollapsed ? 'rotate(-90deg)' : 'none', display: 'inline-block' }}
            aria-expanded={!configSectionCollapsed}
          >
            ▼
          </span>
        </h2>
        <div className={`collapsible ${configSectionCollapsed ? '' : 'open'}`}>
          <p className="helper-text">Load, review, and update the server configuration file from here.</p>
          <div className="action-row">
          <div className="action-details">
            <h3>Server Configuration</h3>
            <p>Review and edit advanced options. Changes are validated before saving.</p>
          </div>
          <div className="action-buttons">
            <button className="ghost" onClick={handleReload}>
              Reload
            </button>
            <button className="primary" disabled={!hasModifications || hasErrors} onClick={handleSave}>
              Save Changes
            </button>
          </div>
        </div>
        <div className="config-panel-content">
          <div className="config-properties">
            <PurityOrderSection />
            {Object.entries(groups).map(([category, properties]) => (
              <div key={category} className={`config-group ${collapsedGroups.has(category) ? 'collapsed' : ''}`}>
                <div className="config-group-header" onClick={() => toggleGroup(category)}>
                  <h3 className="config-group-title">{category}</h3>
                  <span className="config-group-toggle">▼</span>
                </div>
                <div className="config-group-properties">
                  {properties.map(({ key, value }) => {
                    const type = getPropertyType(value);
                    const isModified = modifiedProperties.has(key);
                    const error = validationErrors.get(key);
                    
                    return (
                      <div
                        key={key}
                        className={`config-property ${isModified ? 'modified' : ''} ${error ? 'error' : ''}`}
                        data-key={key}
                      >
                        <div className="config-property-label">{key}</div>
                        <div className="config-property-input-container">
                          {type === 'boolean' ? (
                            <input
                              type="checkbox"
                              className="config-property-checkbox"
                              checked={value as boolean}
                              onChange={(e) => {
                                handlePropertyChange(key, e.target.checked);
                                setConfig((prev) => (prev ? { ...prev, [key]: e.target.checked } : null));
                              }}
                            />
                          ) : (
                            <input
                              type={type === 'number' ? 'number' : 'text'}
                              className="config-property-input"
                              value={String(value)}
                              onChange={(e) => {
                                const newValue = type === 'number' ? Number(e.target.value) : e.target.value;
                                handlePropertyChange(key, newValue);
                                setConfig((prev) => (prev ? { ...prev, [key]: newValue } : null));
                              }}
                              onBlur={(e) => validateProperty(key, e.target.value)}
                            />
                          )}
                        </div>
                        {error && <div className="config-property-error">{error}</div>}
                      </div>
                    );
                  })}
                </div>
              </div>
            ))}
          </div>
        </div>
        </div>
      </section>
      <AdvancedSection />
    </section>
  );
};

export default SettingsPanel;
