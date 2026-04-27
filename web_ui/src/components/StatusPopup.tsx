import React from 'react';
import { useStatusPopupStore } from '../stores';

const StatusPopup: React.FC = () => {
  const { isOpen, result, oldPath, newPath, verified, close } = useStatusPopupStore();

  if (!isOpen || !result) return null;

  const isSuccess = result.status === 'ok';
  const isPartial = result.status === 'partial_success';
  const isError = result.status === 'error';

  const getStatusIcon = () => {
    if (isSuccess && verified) return '✓';
    if (isPartial) return '⚠';
    if (isError) return '✗';
    return '?';
  };

  const getStatusTitle = () => {
    if (isSuccess && verified) return 'Remap Successful';
    if (isSuccess && !verified) return 'Remap Completed (Verification Pending)';
    if (isPartial) return 'Remap Partially Successful';
    if (isError) return 'Remap Failed';
    return 'Remap Status';
  };

  const getStatusColor = () => {
    if (isSuccess && verified) return 'var(--success)';
    if (isPartial) return 'var(--warning)';
    if (isError) return 'var(--danger)';
    return 'var(--muted)';
  };

  return (
    <div className="modal open">
      <div className="modal-backdrop" onClick={close}></div>
      <div className="modal-dialog" style={{ maxWidth: '600px' }}>
        <h2 style={{ color: getStatusColor() }}>
          <span style={{ fontSize: '28px', marginRight: '12px' }}>{getStatusIcon()}</span>
          {getStatusTitle()}
        </h2>
        
        <div style={{ display: 'flex', flexDirection: 'column', gap: '16px' }}>
          <div>
            <strong>Old Path:</strong>
            <div style={{ 
              marginTop: '4px', 
              padding: '8px', 
              background: 'var(--hover-bg)', 
              borderRadius: '8px',
              wordBreak: 'break-all',
              fontSize: '13px'
            }}>
              {oldPath}
            </div>
          </div>
          
          <div>
            <strong>New Path:</strong>
            <div style={{ 
              marginTop: '4px', 
              padding: '8px', 
              background: 'var(--hover-bg)', 
              borderRadius: '8px',
              wordBreak: 'break-all',
              fontSize: '13px'
            }}>
              {newPath}
            </div>
          </div>

          {result.files_updated !== undefined && (
            <div>
              <strong>Files Updated:</strong> {result.files_updated}
            </div>
          )}

          {result.files_failed !== undefined && result.files_failed > 0 && (
            <div style={{ color: 'var(--danger)' }}>
              <strong>Files Failed:</strong> {result.files_failed}
            </div>
          )}

          {result.verification_success_rate !== undefined && (
            <div>
              <strong>Verification Rate:</strong>{' '}
              {(result.verification_success_rate * 100).toFixed(1)}%
            </div>
          )}

          {verified && (
            <div style={{ 
              padding: '12px', 
              background: 'rgba(52, 199, 89, 0.1)', 
              borderRadius: '8px',
              color: 'var(--success)',
              fontWeight: 500
            }}>
              ✓ Verification successful: Path change confirmed on server
            </div>
          )}

          {result.error && (
            <div style={{ 
              padding: '12px', 
              background: 'rgba(225, 29, 72, 0.1)', 
              borderRadius: '8px',
              color: 'var(--danger)',
              fontSize: '13px'
            }}>
              {result.error}
            </div>
          )}

          {result.update_details && (
            <details style={{ marginTop: '8px' }}>
              <summary style={{ cursor: 'pointer', fontWeight: 500, marginBottom: '8px' }}>
                Update Details
              </summary>
              <div style={{ 
                padding: '12px', 
                background: 'var(--hover-bg)', 
                borderRadius: '8px',
                fontSize: '12px',
                display: 'flex',
                flexDirection: 'column',
                gap: '6px'
              }}>
                {result.update_details.scanned_files && (
                  <div>Scanned Files: {result.update_details.scanned_files.updated} updated, {result.update_details.scanned_files.failed} failed</div>
                )}
                {result.update_details.image_artifacts && (
                  <div>Image Artifacts: {result.update_details.image_artifacts.updated} updated, {result.update_details.image_artifacts.failed} failed</div>
                )}
                {result.update_details.duplicate_groups && (
                  <div>Duplicate Groups: {result.update_details.duplicate_groups.updated} updated, {result.update_details.duplicate_groups.failed} failed</div>
                )}
                {result.update_details.duplicate_members && (
                  <div>Duplicate Members: {result.update_details.duplicate_members.updated} updated, {result.update_details.duplicate_members.failed} failed</div>
                )}
                {result.update_details.thumbnail_cache && (
                  <div>Thumbnail Cache: {result.update_details.thumbnail_cache.updated} updated, {result.update_details.thumbnail_cache.failed} failed</div>
                )}
              </div>
            </details>
          )}
        </div>

        <div style={{ display: 'flex', justifyContent: 'flex-end', marginTop: '8px' }}>
          <button className="primary" onClick={close}>
            Close
          </button>
        </div>
      </div>
    </div>
  );
};

export default StatusPopup;
