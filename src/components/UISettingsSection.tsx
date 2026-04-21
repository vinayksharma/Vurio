import React, { useState, useEffect } from 'react';
import { fetchUserSetting, saveUserSetting } from '../utils/api';

const UISettingsSection: React.FC = () => {
  const [theme, setTheme] = useState<string>('os');
  const [isSaving, setIsSaving] = useState(false);

  useEffect(() => {
    let mounted = true;
    fetchUserSetting('ui_theme')
      .then((val) => {
        if (!mounted) return;
        if (val) {
          setTheme(val);
        }
      })
      .catch((err) => console.error('Failed to load UI theme from backend:', err));
    return () => { mounted = false; };
  }, []);

  const handleThemeChange = async (e: React.ChangeEvent<HTMLSelectElement>) => {
    const newTheme = e.target.value;
    setTheme(newTheme);
    setIsSaving(true);
    
    // Apply actively to document
    if (newTheme === 'os') {
      document.documentElement.removeAttribute('data-theme');
    } else {
      document.documentElement.setAttribute('data-theme', newTheme);
    }

    try {
      await saveUserSetting('ui_theme', newTheme);
    } catch (err) {
      console.error('Failed to save UI theme:', err);
    } finally {
      setIsSaving(false);
    }
  };

  return (
    <section className="panel settings-panel">
      <div className="panel-title split">
        <span>UI Settings</span>
      </div>
      <div>
        <p className="helper-text" style={{ marginBottom: '16px' }}>
          Customize the appearance of VURIO to match your preferences.
        </p>
        
        <div className="field-row">
          <div className="field-column" style={{ width: '100%' }}>
            <label style={{ fontSize: '12px', fontWeight: 600, color: 'var(--text)' }}>Theme Preference</label>
            <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
              <select
                value={theme}
                onChange={handleThemeChange}
                disabled={isSaving}
                className="config-property-input"
                style={{ 
                  flex: 1, 
                  height: '28px', 
                  borderRadius: '5px', 
                  borderColor: 'var(--panel-border)', 
                  backgroundColor: 'var(--input-bg)', 
                  color: 'var(--text)' 
                }}
              >
                <option value="os">OS Default</option>
                <option value="light">Light Mode</option>
                <option value="dark">Dark Mode</option>
              </select>
              {isSaving && <span style={{ fontSize: '12px', color: 'var(--muted)' }}>Saving...</span>}
            </div>
            <span style={{ fontSize: '11px', color: 'var(--muted)' }}>Select the application theme or automatically follow your system preference.</span>
          </div>
        </div>
      </div>
    </section>
  );
};

export default UISettingsSection;
