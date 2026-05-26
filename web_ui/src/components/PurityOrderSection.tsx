import React, { useState, useEffect } from 'react';
import { fetchConfiguration, configurePurityOrder } from '../utils/api';

const DEFAULT_PURITY_ORDER = ["CAMERA_RAW", "EXIF_CAPTURE_DATE", "FILE_SIZE"];

const PurityOrderSection: React.FC = () => {
  const [items, setItems] = useState<string[]>(DEFAULT_PURITY_ORDER);
  const [originalItems, setOriginalItems] = useState<string[]>(DEFAULT_PURITY_ORDER);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [showWarning, setShowWarning] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [successMsg, setSuccessMsg] = useState<string | null>(null);
  const [isCollapsed, setIsCollapsed] = useState(true);

  useEffect(() => {
    loadConfig();
  }, []);

  const loadConfig = async () => {
    try {
      const config = await fetchConfiguration();
      const orderStr = config['duplicates.purity.order'] as string;
      if (orderStr) {
        const parts = orderStr.split(',').map(s => s.trim()).filter(Boolean);
        if (parts.length > 0) {
          setItems(parts);
          setOriginalItems(parts);
        }
      }
    } catch (err) {
      console.error('Failed to load purity config', err);
    } finally {
      setLoading(false);
    }
  };

  const moveUp = (index: number) => {
    if (index === 0) return;
    const newItems = [...items];
    const temp = newItems[index - 1];
    newItems[index - 1] = newItems[index];
    newItems[index] = temp;
    setItems(newItems);
    setSuccessMsg(null);
  };

  const moveDown = (index: number) => {
    if (index === items.length - 1) return;
    const newItems = [...items];
    const temp = newItems[index + 1];
    newItems[index + 1] = newItems[index];
    newItems[index] = temp;
    setItems(newItems);
    setSuccessMsg(null);
  };

  const hasChanges = items.join(',') !== originalItems.join(',');

  const handleSaveConfirmed = async () => {
    setShowWarning(false);
    setSaving(true);
    setError(null);
    setSuccessMsg(null);
    try {
      const resp = await configurePurityOrder(items.join(', '));
      if (resp.status === 'ok') {
        setSuccessMsg(resp.message);
        setOriginalItems([...items]);
      } else {
        throw new Error(resp.error || 'Unknown error');
      }
    } catch (err: any) {
      setError(err.message || String(err));
    } finally {
      setSaving(false);
    }
  };

  return (
    <section 
      className="panel settings-panel"
      onClick={() => {
        if (isCollapsed) setIsCollapsed(false);
      }}
      style={{ cursor: isCollapsed ? 'pointer' : 'default' }}
    >
      <h2 
        className="panel-title" 
        onClick={(e) => {
          setIsCollapsed(!isCollapsed);
          e.stopPropagation();
        }} 
        style={{ cursor: 'pointer' }}
      >
        <span>Duplicate Purity Rules</span>
        <span 
          className="config-group-toggle" 
          style={{ transform: isCollapsed ? 'rotate(-90deg)' : 'none', display: 'inline-block' }}
          aria-expanded={!isCollapsed}
        >
          ▼
        </span>
      </h2>
      <div className={`collapsible ${isCollapsed ? '' : 'open'}`}>
        <p className="helper-text">
          Customize the rules the detection engine uses to determine which file in a duplicate group should be retained as the original representation. 
          The top-most rule is evaluated first.
        </p>

        {error && <div className="error-banner" style={{ margin: '10px 0', padding: '10px', backgroundColor: '#fee2e2', color: '#991b1b', borderRadius: '4px' }}>{error}</div>}
        {successMsg && <div className="success-banner" style={{ margin: '10px 0', padding: '10px', backgroundColor: '#dcfce7', color: '#166534', borderRadius: '4px' }}>{successMsg}</div>}

        <div className="purity-order-list" style={{ display: 'flex', flexDirection: 'column', gap: '8px', margin: '15px 0' }}>
          {loading ? (
             <div className="config-loading">Loading...</div>
          ) : (
            items.map((item, index) => (
              <div key={item} style={{ display: 'flex', alignItems: 'center', background: 'var(--surface-color)', padding: '10px', border: '1px solid var(--border-color)', borderRadius: '6px' }}>
                <div style={{ fontWeight: '500', display: 'flex', alignItems: 'center', gap: '10px', flex: 1 }}>
                  <span style={{ display: 'inline-flex', alignItems: 'center', justifyContent: 'center', width: '24px', height: '24px', background: 'var(--primary-color)', color: 'white', borderRadius: '50%', fontSize: '12px' }}>
                    {index + 1}
                  </span>
                  {item.replace(/_/g, ' ')}
                </div>
                <div style={{ display: 'flex', gap: '4px' }}>
                  <button className="ghost small" disabled={index === 0} onClick={() => moveUp(index)}>↑</button>
                  <button className="ghost small" disabled={index === items.length - 1} onClick={() => moveDown(index)}>↓</button>
                </div>
              </div>
            ))
          )}
        </div>

        <div className="action-row" style={{ marginTop: '20px', justifyContent: 'flex-end', display: 'flex' }}>
          <button 
            className="primary" 
            disabled={!hasChanges || saving} 
            onClick={() => setShowWarning(true)}
          >
            {saving ? 'Applying...' : 'Apply Purity Order'}
          </button>
        </div>

        {/* Warning Modal */}
        {showWarning && (
          <div className="modal-overlay" style={{ position: 'fixed', top: 0, left: 0, right: 0, bottom: 0, background: 'rgba(0,0,0,0.6)', zIndex: 1000, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
            <div className="modal-content" style={{ background: 'var(--bg-color)', padding: '24px', borderRadius: '8px', maxWidth: '400px', width: '100%', boxShadow: '0 10px 25px rgba(0,0,0,0.2)' }}>
              <h3 style={{ marginTop: 0, color: '#dc2626' }}>Warning: Destructive Operation</h3>
              <p>
                Changing the purity rules requires completely clearing all currently identified duplicate groups. 
                Additionally, all ejected items will be bulk-restored. 
              </p>
              <p>
                The background scanner will then automatically reconstruct your duplicate groups from scratch using your new rules. 
              </p>
              <p><strong>Are you sure you want to proceed?</strong></p>
              <div style={{ display: 'flex', justifyContent: 'flex-end', gap: '10px', marginTop: '20px' }}>
                <button className="ghost" onClick={() => setShowWarning(false)}>Cancel</button>
                <button className="primary" style={{ background: '#dc2626', borderColor: '#dc2626' }} onClick={handleSaveConfirmed}>Wipe & Rebuild</button>
              </div>
            </div>
          </div>
        )}
      </div>
    </section>
  );
};

export default PurityOrderSection;
