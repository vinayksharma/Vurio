import React from 'react';
import Sidebar from './Sidebar';
import type { PanelType } from '../types';

interface SidebarControlProps {
  activePanel: PanelType;
  onPanelChange: (panel: PanelType) => void;
  width?: number;
  height?: number;
  isVertical?: boolean;
  isOpen?: boolean;
  onClose?: () => void;
}

const SidebarControl: React.FC<SidebarControlProps> = ({
  activePanel,
  onPanelChange,
  width,
  height,
  isVertical = false,
  isOpen = false,
  onClose,
}) => {
  const handlePanelChange = (panel: PanelType) => {
    onPanelChange(panel);
    if (isVertical && onClose) {
      onClose(); // Close sidebar after selection in vertical mode
    }
  };

  const style: React.CSSProperties = {
    width: width !== undefined ? `${width}px` : undefined,
    height: height !== undefined ? `${height}px` : undefined,
    flexShrink: 0,
    overflow: 'auto',
  };

  if (isVertical) {
    if (!isOpen) return null;
    
    return (
      <>
        <div className="sidebar-overlay" onClick={onClose} />
        <div
          className="sidebar-control sidebar-control-vertical"
          style={style}
          onClick={(e) => e.stopPropagation()}
        >
          <Sidebar activePanel={activePanel} onPanelChange={handlePanelChange} />
        </div>
      </>
    );
  }

  return (
    <div className="sidebar-control" style={style}>
      <Sidebar activePanel={activePanel} onPanelChange={onPanelChange} />
    </div>
  );
};

export default SidebarControl;
