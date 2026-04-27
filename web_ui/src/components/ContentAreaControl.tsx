import React from 'react';
import DuplicatesPanel from './DuplicatesPanel';
import SettingsPanel from './SettingsPanel';
import StatusBar from './StatusBar';
import StatusPopup from './StatusPopup';
import type { PanelType } from '../types';

interface ContentAreaControlProps {
  activePanel: PanelType;
  flex?: number;
  height?: number;
  isVertical?: boolean;
  onHamburgerClick?: () => void;
}

const ContentAreaControl: React.FC<ContentAreaControlProps> = ({
  activePanel,
  flex,
  height,
  isVertical = false,
  onHamburgerClick,
}) => {
  const style: React.CSSProperties = {
    flex: flex !== undefined ? flex : 1,
    height: height !== undefined ? `${height}px` : undefined,
    minWidth: 0,
    minHeight: 0,
    overflow: 'hidden',
    position: 'relative',
  };

  return (
    <div className="content-area-control" style={style}>
      {isVertical && onHamburgerClick && (
        <button
          className="hamburger-button"
          onClick={onHamburgerClick}
          aria-label="Toggle menu"
        >
          <span className="hamburger-icon">
            <span></span>
            <span></span>
            <span></span>
          </span>
        </button>
      )}
      <div className="content-area">
        <main className="app-main">
          <DuplicatesPanel isActive={activePanel === 'duplicates'} />
          <SettingsPanel isActive={activePanel === 'settings'} />
        </main>
        <StatusBar />
      </div>
      <StatusPopup />
    </div>
  );
};

export default ContentAreaControl;
