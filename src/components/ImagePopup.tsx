import React, { useState, useRef, useEffect } from 'react';
import { useImagePopupStore } from '../stores';
import { API_BASE } from '../utils/api';

const ImagePopup: React.FC = () => {
  const { isOpen, filePath, details, close } = useImagePopupStore();
  const [scale, setScale] = useState(1);
  const [isDragging, setIsDragging] = useState(false);
  const [offset, setOffset] = useState({ x: 0, y: 0 });
  const [showDetails, setShowDetails] = useState(false);
  const [isLoading, setIsLoading] = useState(true);
  
  // Track dragging operation explicitly
  const dragStateRef = useRef({ 
    startX: 0, 
    startY: 0, 
    imgStartX: 0, 
    imgStartY: 0, 
    isDragging: false 
  });
  
  // Reset state when a new image opens
  useEffect(() => {
    if (isOpen) {
      setScale(1);
      setOffset({ x: 0, y: 0 });
      setShowDetails(false);
      setIsLoading(true);
    }
  }, [isOpen, filePath]);

  // Keyboard navigation
  useEffect(() => {
    if (!isOpen) return;
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') handleClose();
      if (e.key === '=' || e.key === '+') setScale(s => Math.min(s * 1.5, 10));
      if (e.key === '-') setScale(s => Math.max(s / 1.5, 0.1));
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [isOpen]);

  const buildImageUrl = (path: string): string => {
    // Request a larger thumbnail image for zooming (backend max size is 1024)
    return `${API_BASE}/api/v1/thumbnails?path=${encodeURIComponent(path)}&size=1024`;
  };

  const handleMouseDown = (e: React.MouseEvent) => {
    if (e.button !== 0) return; // Only process left clicks
    e.preventDefault();
    
    dragStateRef.current = {
      startX: e.clientX,
      startY: e.clientY,
      imgStartX: offset.x,
      imgStartY: offset.y,
      isDragging: true
    };
    setIsDragging(true);
  };

  const handleMouseMove = (e: React.MouseEvent) => {
    if (!dragStateRef.current.isDragging) return;
    
    const dx = e.clientX - dragStateRef.current.startX;
    const dy = e.clientY - dragStateRef.current.startY;
    
    setOffset({
      x: dragStateRef.current.imgStartX + dx,
      y: dragStateRef.current.imgStartY + dy,
    });
  };

  const handleMouseUp = (e: React.MouseEvent) => {
    if (!dragStateRef.current.isDragging) return;
    dragStateRef.current.isDragging = false;
    setIsDragging(false);

    const dx = e.clientX - dragStateRef.current.startX;
    const dy = e.clientY - dragStateRef.current.startY;
    const distance = Math.hypot(dx, dy);

    // If it was a small movement, treat is as a click instead of a drag
    if (distance < 5) {
      if (scale === 1) {
        setScale(2.5); // Zoom in on click
      } else {
        setScale(1);   // Reset to fit on click
        setOffset({ x: 0, y: 0 });
      }
    }
  };

  const handleWheel = (e: React.WheelEvent) => {
    const zoomSensitivity = 0.003;
    const zoomFactor = 1 - Math.sign(e.deltaY) * Math.min(Math.abs(e.deltaY), 50) * zoomSensitivity;
    
    let newScale = scale * zoomFactor;
    newScale = Math.min(Math.max(0.1, newScale), 10);
    setScale(newScale);
  };

  const handleClose = () => {
    close();
  };

  const copyPath = () => {
    navigator.clipboard.writeText(filePath);
  };

  if (!isOpen) return null;

  return (
    <div className="image-preview-modal open">
      <div className="image-preview-backdrop" onClick={handleClose}></div>
      <div className="image-preview-dialog">
        <div className="image-preview-header">
          <div className="image-preview-title">Image Viewer</div>
          <button className="image-preview-close" onClick={handleClose} title="Close (Esc)">
            ✕
          </button>
        </div>
        
        <div className="image-preview-content" onWheel={handleWheel}>
          <div
            className={`image-preview-viewport ${isDragging ? 'cursor-grabbing' : 'cursor-grab'}`}
            onMouseDown={handleMouseDown}
            onMouseMove={handleMouseMove}
            onMouseUp={handleMouseUp}
            onMouseLeave={handleMouseUp}
          >
            {isLoading && (
              <div style={{ position: 'absolute', top: '50%', left: '50%', transform: 'translate(-50%, -50%)', zIndex: 10 }}>
                <div className="thumbnail-loading-spinner" style={{ width: 40, height: 40, borderWidth: 3 }}></div>
              </div>
            )}
            <img
              className="image-preview-image"
              src={buildImageUrl(filePath)}
              alt="Preview"
              style={{
                transform: `translate(calc(-50% + ${offset.x}px), calc(-50% + ${offset.y}px)) scale(${scale})`,
                transition: isDragging ? 'none' : 'transform 0.15s ease-out',
                opacity: isLoading ? 0 : 1
              }}
              onDragStart={(e) => e.preventDefault()}
              onLoad={() => setIsLoading(false)}
              onError={() => setIsLoading(false)}
            />
          </div>
          
          <div className="details-overlay">
            <button 
              className="details-toggle-btn" 
              onClick={() => setShowDetails(!showDetails)}
              title={showDetails ? "Hide metadata" : "Show metadata"}
            >
              {showDetails ? '▼ Hide Info' : '▲ Show Info'}
            </button>
            {showDetails && (
              <div className="details-content">
                {details}
              </div>
            )}
          </div>
        </div>
        
        <div className="image-preview-toolbar-container">
          <div className="image-preview-toolbar">
            <button onClick={() => setScale(s => Math.max(s / 1.5, 0.1))} title="Zoom Out (-)">-</button>
            <button onClick={() => { setScale(1); setOffset({x:0, y:0}); }} title="Fit to Screen">Reset Zoom</button>
            <button onClick={() => setScale(s => Math.min(s * 1.5, 10))} title="Zoom In (+)">+</button>
            <div className="toolbar-divider"></div>
            <button onClick={copyPath} title="Copy file path to clipboard">Copy Path</button>
          </div>
        </div>
      </div>
    </div>
  );
};

export default ImagePopup;
