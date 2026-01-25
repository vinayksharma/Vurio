import React, { useState, useRef } from 'react';
import { useImagePopupStore } from '../stores';
import { API_BASE } from '../utils/api';

const ImagePopup: React.FC = () => {
  const { isOpen, filePath, details, close } = useImagePopupStore();
  const [isZoomed, setIsZoomed] = useState(false);
  const [isDragging, setIsDragging] = useState(false);
  const [imgOffset, setImgOffset] = useState({ x: 0, y: 0 });
  const dragStartRef = useRef({ x: 0, y: 0 });
  const imgStartRef = useRef({ x: 0, y: 0 });
  const viewportRef = useRef<HTMLDivElement>(null);
  const imgRef = useRef<HTMLImageElement>(null);

  const buildImageUrl = (filePath: string): string => {
    return `${API_BASE}/api/v1/thumbnails?path=${encodeURIComponent(filePath)}&size=1024`;
  };

  const handleMouseDown = (e: React.MouseEvent) => {
    if (!isZoomed) return;
    setIsDragging(true);
    dragStartRef.current = { x: e.clientX, y: e.clientY };
    e.preventDefault();
  };

  const handleMouseMove = (e: React.MouseEvent) => {
    if (!isDragging) return;
    const dx = e.clientX - dragStartRef.current.x;
    const dy = e.clientY - dragStartRef.current.y;
    setImgOffset({
      x: imgStartRef.current.x + dx,
      y: imgStartRef.current.y + dy,
    });
  };

  const handleMouseUp = (e: React.MouseEvent) => {
    if (isDragging) {
      const dx = e.clientX - dragStartRef.current.x;
      const dy = e.clientY - dragStartRef.current.y;
      imgStartRef.current = {
        x: imgStartRef.current.x + dx,
        y: imgStartRef.current.y + dy,
      };
      setIsDragging(false);
      return;
    }

    if (e.target === viewportRef.current || e.target === imgRef.current) {
      setIsZoomed(!isZoomed);
      if (!isZoomed) {
        imgStartRef.current = { x: 0, y: 0 };
        setImgOffset({ x: 0, y: 0 });
      }
    }
  };

  const handleClose = () => {
    setIsZoomed(false);
    setIsDragging(false);
    setImgOffset({ x: 0, y: 0 });
    imgStartRef.current = { x: 0, y: 0 };
    close();
  };

  if (!isOpen) return null;

  const cursorClass = isDragging
    ? 'cursor-grabbing'
    : isZoomed
    ? 'cursor-zoom-out'
    : 'cursor-zoom-in';

  return (
    <div className="image-preview-modal open">
      <div className="image-preview-backdrop" onClick={handleClose}></div>
      <div className="image-preview-dialog">
        <div className="image-preview-header">
          <button className="image-preview-close" onClick={handleClose} title="Close">
            ✕
          </button>
        </div>
        <div className="image-preview-content">
          <div
            ref={viewportRef}
            className={`image-preview-viewport ${cursorClass}`}
            onMouseDown={handleMouseDown}
            onMouseMove={handleMouseMove}
            onMouseUp={handleMouseUp}
            onMouseLeave={() => setIsDragging(false)}
          >
            <img
              ref={imgRef}
              className="image-preview-image"
              src={buildImageUrl(filePath)}
              alt="Preview"
              style={{
                transform: isZoomed
                  ? `translate(calc(-50% + ${imgOffset.x}px), calc(-50% + ${imgOffset.y}px)) scale(1)`
                  : 'translate(-50%, -50%) scale(1)',
                maxWidth: isZoomed ? 'none' : '100%',
                maxHeight: isZoomed ? 'none' : '100%',
              }}
              onDragStart={(e) => e.preventDefault()}
            />
          </div>
        </div>
        <div className="image-preview-footer">{details}</div>
      </div>
    </div>
  );
};

export default ImagePopup;
