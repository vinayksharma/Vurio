import React, { useState } from 'react';
import { resetErrors, resetDuplicates } from '../utils/api';

interface AdvancedPanelProps {
  isActive: boolean;
}

const AdvancedPanel: React.FC<AdvancedPanelProps> = ({ isActive }) => {
  const [isResettingErrors, setIsResettingErrors] = useState(false);
  const [isResettingDuplicates, setIsResettingDuplicates] = useState(false);
  const [isScrollTestRunning, setIsScrollTestRunning] = useState(false);

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

  const handleToggleDevTools = () => {
    if (window.electronAPI?.toggleDevTools) {
      window.electronAPI.toggleDevTools();
    } else {
      alert('Developer Tools are only available in Electron');
    }
  };

  const handleStartScrollTest = async () => {
    if (isScrollTestRunning) {
      // Stop the test
      if (window.stopScrollTest) {
        window.stopScrollTest();
      }
      setIsScrollTestRunning(false);
      return;
    }

    // Ensure duplicates panel is active first
    const duplicatesBtn = Array.from(document.querySelectorAll('button')).find((b) => 
      b.textContent?.includes('Duplicates') || b.textContent?.includes('🫂')
    ) as HTMLButtonElement | undefined;
    
    if (duplicatesBtn) {
      duplicatesBtn.click();
      // Wait a bit for panel to activate
      await new Promise(resolve => setTimeout(resolve, 1000));
    }

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
    <section id="panel-advanced" className={`content-panel ${isActive ? 'active' : ''}`}>
      <header className="panel-header">
        <div>
          <h1>Advanced</h1>
          <p className="panel-description">
            Maintenance shortcuts and developer tools for troubleshooting the server.
          </p>
        </div>
      </header>
      <section className="panel advanced-panel">
        <div className="action-row">
          <div className="action-details">
            <h3>Reset Error Files</h3>
            <p>Clear the list of files that previously failed processing so they can be scanned again.</p>
          </div>
          <button
            id="reset-errors-btn"
            className="ghost danger"
            onClick={handleResetErrors}
            disabled={isResettingErrors}
          >
            {isResettingErrors ? 'Resetting...' : 'Reset Errors'}
          </button>
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
            disabled={isResettingDuplicates}
          >
            {isResettingDuplicates ? 'Resetting...' : 'Reset Duplicates'}
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
            className={isScrollTestRunning ? "ghost danger" : "ghost"} 
            onClick={handleStartScrollTest}
          >
            {isScrollTestRunning ? 'Stop Test' : 'Start Scroll Test'}
          </button>
        </div>
      </section>
    </section>
  );
};

export default AdvancedPanel;
