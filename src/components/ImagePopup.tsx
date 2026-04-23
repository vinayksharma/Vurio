import React, { useState, useRef, useEffect } from 'react';
import { API_BASE, ejectDuplicates, undoEjectDuplicates } from '../utils/api';

const IconButton = ({ onClick, title, children }: { onClick: () => void, title: string, children: React.ReactNode }) => {
  const [isHovered, setIsHovered] = useState(false);
  const [isPressed, setIsPressed] = useState(false);
  return (
    <button
      onClick={onClick}
      title={title}
      onMouseEnter={() => setIsHovered(true)}
      onMouseLeave={() => { setIsHovered(false); setIsPressed(false); }}
      onMouseDown={() => setIsPressed(true)}
      onMouseUp={() => setIsPressed(false)}
      style={{
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        width: '36px',
        height: '36px',
        minHeight: '36px',
        padding: 0,
        borderRadius: '50%',
        background: isPressed ? 'rgba(255, 255, 255, 0.2)' : isHovered ? 'rgba(255, 255, 255, 0.1)' : 'transparent',
        border: 'none',
        color: isHovered ? '#ffffff' : 'rgba(255, 255, 255, 0.85)',
        cursor: 'pointer',
        transition: 'all 0.15s ease',
        transform: isPressed ? 'scale(0.92)' : 'scale(1)',
      }}
    >
      {children}
    </button>
  );
};

const ImagePopup: React.FC = () => {
  const [filePath, setFilePath] = useState('');
  const [details, setDetails] = useState('');
  
  const [scale, setScale] = useState(1);
  const [rotation, setRotation] = useState(0);
  const [isDragging, setIsDragging] = useState(false);
  const [offset, setOffset] = useState({ x: 0, y: 0 });
  const [showDetails, setShowDetails] = useState(false);
  const [isLoading, setIsLoading] = useState(true);
  const [isEjecting, setIsEjecting] = useState(false);
  
  const [groupImages, setGroupImages] = useState<{filePath: string, details: string, is_ejected?: boolean}[]>([]);
  const [currentIndex, setCurrentIndex] = useState<number>(0);
  const [nativeUrl, setNativeUrl] = useState<string | null>(null);
  
  // Track dragging operation explicitly
  const dragStateRef = useRef({ 
    startX: 0, 
    startY: 0, 
    imgStartX: 0, 
    imgStartY: 0, 
    isDragging: false 
  });
  
  const bcRef = useRef<BroadcastChannel | null>(null);

  useEffect(() => {
    bcRef.current = new BroadcastChannel('viewer_group_navigation');
    bcRef.current.onmessage = (e) => {
      if (e.data.type === 'NEW_GROUP') {
        setGroupImages(e.data.groupImages);
        setCurrentIndex(e.data.currentIndex);
        setNativeUrl(null);
        setIsFetchingNative(false);
        setFilePath(e.data.filePath);
        setDetails(e.data.details);
        setIsLoading(true);
        setScale(1);
        setOffset({ x: 0, y: 0 });
        setRotation(0);
      }
    };
    return () => {
      bcRef.current?.postMessage({ type: 'VIEWER_CLOSED' });
      bcRef.current?.close();
    };
  }, []);

  useEffect(() => {
    // Read from URL search params since it's a standalone window now
    const params = new URLSearchParams(window.location.search);
    const urlFilePath = params.get('filePath') || '';
    const urlDetails = params.get('details') || '';
    
    let storedGroupImages: {filePath: string, details: string, is_ejected?: boolean}[] = [];
    let storedIndex = 0;
    
    try {
      const storedData = localStorage.getItem('viewer_group_images');
      if (storedData) {
        const parsed = JSON.parse(storedData);
        if (parsed.groupImages && Array.isArray(parsed.groupImages)) {
          storedGroupImages = parsed.groupImages;
          storedIndex = parsed.currentIndex || 0;
        }
      }
    } catch (e) {
      console.error('Failed to parse viewer group images', e);
    }
    
    if (storedGroupImages.length > 0) {
      setGroupImages(storedGroupImages);
      setCurrentIndex(storedIndex);
      setFilePath(storedGroupImages[storedIndex]?.filePath || urlFilePath);
      setDetails(storedGroupImages[storedIndex]?.details || urlDetails);
    } else {
      setFilePath(urlFilePath);
      setDetails(urlDetails);
    }
  }, []);

  const navigateTo = (index: number) => {
    if (groupImages.length === 0) return;
    let newIndex = index;
    if (newIndex < 0) newIndex = groupImages.length - 1;
    if (newIndex >= groupImages.length) newIndex = 0;
    setCurrentIndex(newIndex);
    setNativeUrl(null);
    setIsFetchingNative(false);
    setFilePath(groupImages[newIndex].filePath);
    setDetails(groupImages[newIndex].details);
    setIsLoading(true);
  };

  const handlePrev = () => navigateTo(currentIndex - 1);
  const handleNext = () => navigateTo(currentIndex + 1);

  // Keyboard navigation
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') handleClose();
      if (e.key === '=' || e.key === '+') setScale(s => Math.min(s * 1.5, 10));
      if (e.key === '-') setScale(s => Math.max(s / 1.5, 0.1));
      if (e.key === '0') { setScale(1); setOffset({x: 0, y: 0}); }
      if (e.key === ']') setRotation(r => r + 90);
      if (e.key === '[') setRotation(r => r - 90);
      if (e.key === 'ArrowLeft') handlePrev();
      if (e.key === 'ArrowRight') handleNext();
      if (e.key === 'ArrowUp') bcRef.current?.postMessage({ type: 'NAVIGATE_GROUP', direction: -1, currentFilePath: filePath });
      if (e.key === 'ArrowDown') bcRef.current?.postMessage({ type: 'NAVIGATE_GROUP', direction: 1, currentFilePath: filePath });
      if (e.key === 'PageUp') {
        e.preventDefault();
        bcRef.current?.postMessage({ type: 'NAVIGATE_GROUP', jumpType: 'page', direction: -1, currentFilePath: filePath });
      }
      if (e.key === 'PageDown') {
        e.preventDefault();
        bcRef.current?.postMessage({ type: 'NAVIGATE_GROUP', jumpType: 'page', direction: 1, currentFilePath: filePath });
      }
      if (e.key === 'Backspace' || e.key === 'Delete') {
        const currentImage = groupImages[currentIndex];
        const isRepresentative = currentIndex === 0;
        const isEjected = currentImage?.is_ejected || false;
        if (!isRepresentative && groupImages.length > 0) {
          isEjected ? handleRestore() : handleEject();
        }
      }
      if ((e.key === 'c' || e.key === 'C') && (e.metaKey || e.ctrlKey)) {
        e.preventDefault();
        copyImageToClipboard();
      } else if ((e.key === 'c' || e.key === 'C') && !e.metaKey && !e.ctrlKey) {
        copyPath();
      }
      if (e.key === 'i' || e.key === 'I') setShowDetails(prev => !prev);
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [groupImages, currentIndex, filePath]);

  // Track native fetch to prevent scroll-wheel spam
  const fetchingNativePathRef = useRef<string | null>(null);
  const [isFetchingNative, setIsFetchingNative] = useState(false);

  useEffect(() => {
    // Reset our fetch lock when the image changes entirely
    if (filePath !== fetchingNativePathRef.current) {
       fetchingNativePathRef.current = null;
    }

    if (scale > 1.2 && filePath && !nativeUrl && fetchingNativePathRef.current !== filePath) {
      fetchingNativePathRef.current = filePath;
      setIsFetchingNative(true);
      const activeFilePath = filePath;
      const url = `${API_BASE}/api/v1/thumbnails?path=${encodeURIComponent(filePath)}&size=0`;
      const img = new window.Image();
      img.onload = () => {
         if (fetchingNativePathRef.current === activeFilePath) {
           setNativeUrl(url);
           setIsFetchingNative(false);
         }
      };
      img.onerror = () => {
         if (fetchingNativePathRef.current === activeFilePath) {
           // Allow retry on next zoom
           fetchingNativePathRef.current = null;
           setIsFetchingNative(false);
         }
      };
      img.src = url;
    }
  }, [scale, filePath, nativeUrl]);

  const buildImageUrl = (path: string): string => {
    // Request a larger thumbnail image for zooming (backend max size is 1024)
    if (!path) return '';
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
      if (scale === 1 && offset.x === 0 && offset.y === 0) {
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
    window.close(); // Close the native browser window
  };

  const copyPath = () => {
    if (filePath) navigator.clipboard.writeText(filePath);
  };

  const copyImageToClipboard = () => {
    if (!filePath) return;
    try {
      const url = nativeUrl || buildImageUrl(filePath);
      const img = new window.Image();
      img.crossOrigin = 'anonymous';
      img.onload = () => {
        const canvas = document.createElement('canvas');
        canvas.width = img.width;
        canvas.height = img.height;
        const ctx = canvas.getContext('2d');
        if (!ctx) return;
        ctx.drawImage(img, 0, 0);
        canvas.toBlob(blob => {
          if (blob) {
            navigator.clipboard.write([
              new ClipboardItem({ 'image/png': blob })
            ]).catch(console.error);
          }
        }, 'image/png');
      };
      img.src = url;
    } catch (e) {
      console.error('Failed to copy image', e);
    }
  };

  const currentImage = groupImages[currentIndex];
  const isRepresentative = currentIndex === 0;
  const isEjected = currentImage?.is_ejected || false;

  const handleEject = async () => {
    if (!filePath || isEjecting || isRepresentative || isEjected) return;
    setIsEjecting(true);
    try {
      const res = await ejectDuplicates([filePath]);
      if (res.success) {
        setGroupImages(prev => prev.map((img, idx) => 
          idx === currentIndex ? { ...img, is_ejected: true } : img
        ));
        // Also update localStorage so it persists on reload
        try {
          const storedData = localStorage.getItem('viewer_group_images');
          if (storedData) {
            const parsed = JSON.parse(storedData);
            if (parsed.groupImages) {
              parsed.groupImages[currentIndex].is_ejected = true;
              localStorage.setItem('viewer_group_images', JSON.stringify(parsed));
            }
          }
        } catch (e) {}
      }
    } catch (e) {
      console.error(e);
    } finally {
      setIsEjecting(false);
    }
  };

  const handleRestore = async () => {
    if (!filePath || isEjecting || !isEjected) return;
    setIsEjecting(true);
    try {
      const res = await undoEjectDuplicates([filePath]);
      if (res.success) {
        setGroupImages(prev => prev.map((img, idx) => 
          idx === currentIndex ? { ...img, is_ejected: false } : img
        ));
        // Also update localStorage
        try {
          const storedData = localStorage.getItem('viewer_group_images');
          if (storedData) {
            const parsed = JSON.parse(storedData);
            if (parsed.groupImages) {
              parsed.groupImages[currentIndex].is_ejected = false;
              localStorage.setItem('viewer_group_images', JSON.stringify(parsed));
            }
          }
        } catch (e) {}
      }
    } catch (e) {
      console.error(e);
    } finally {
      setIsEjecting(false);
    }
  };

  if (!filePath) return <div style={{ color: 'white', padding: 20 }}>Loading...</div>;

  return (
    <div className="standalone-viewer-container" style={{ width: '100vw', height: '100vh', display: 'flex', flexDirection: 'column', background: '#0f172a', overflow: 'hidden' }}>
      
      <div className="image-preview-content" onWheel={handleWheel} style={{ flex: 1, position: 'relative', overflow: 'hidden' }}>
        <div
          className={`image-preview-viewport ${isDragging ? 'cursor-grabbing' : 'cursor-grab'}`}
          onMouseDown={handleMouseDown}
          onMouseMove={handleMouseMove}
          onMouseUp={handleMouseUp}
          onMouseLeave={handleMouseUp}
          style={{ width: '100%', height: '100%', display: 'flex', alignItems: 'center', justifyContent: 'center' }}
        >
          {isLoading && (
            <div style={{ position: 'absolute', top: '50%', left: '50%', transform: 'translate(-50%, -50%)', zIndex: 10 }}>
              <div className="thumbnail-loading-spinner" style={{ width: 40, height: 40, borderWidth: 3 }}></div>
            </div>
          )}
          <img
            className="image-preview-image"
            src={nativeUrl || buildImageUrl(filePath)}
            alt="Preview"
            style={{
              transform: `translate(calc(-50% + ${offset.x}px), calc(-50% + ${offset.y}px)) scale(${scale}) rotate(${rotation}deg)`,
              transition: isDragging ? 'none' : 'transform 0.15s ease-out',
              opacity: isLoading ? 0 : 1,
              position: 'absolute',
              top: '50%',
              left: '50%',
              maxWidth: Math.abs(rotation % 180) === 90 ? '100vh' : '100%',
              maxHeight: Math.abs(rotation % 180) === 90 ? '100vw' : '100%',
              width: '100%',
              height: '100%',
              objectFit: 'contain',
              boxSizing: 'border-box',
            }}
            onDragStart={(e) => e.preventDefault()}
            onLoad={() => setIsLoading(false)}
            onError={() => setIsLoading(false)}
          />
          
          {groupImages.length > 1 && (
            <>
              <button
                onClick={(e) => { e.stopPropagation(); handlePrev(); }}
                onMouseUp={(e) => e.stopPropagation()}
                onMouseDown={(e) => e.stopPropagation()}
                style={{
                  position: 'absolute',
                  left: '20px',
                  top: '50%',
                  transform: 'translateY(-50%)',
                  background: 'rgba(0,0,0,0.5)',
                  color: 'white',
                  border: 'none',
                  borderRadius: '50%',
                  width: '50px',
                  height: '50px',
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'center',
                  cursor: 'pointer',
                  zIndex: 30,
                  transition: 'background 0.2s',
                }}
                onMouseEnter={(e) => e.currentTarget.style.background = 'rgba(0,0,0,0.8)'}
                onMouseLeave={(e) => e.currentTarget.style.background = 'rgba(0,0,0,0.5)'}
                title="Previous Image (Left Arrow)"
              >
                <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><polyline points="15 18 9 12 15 6"></polyline></svg>
              </button>
              
              <button
                onClick={(e) => { e.stopPropagation(); handleNext(); }}
                onMouseUp={(e) => e.stopPropagation()}
                onMouseDown={(e) => e.stopPropagation()}
                style={{
                  position: 'absolute',
                  right: '20px',
                  top: '50%',
                  transform: 'translateY(-50%)',
                  background: 'rgba(0,0,0,0.5)',
                  color: 'white',
                  border: 'none',
                  borderRadius: '50%',
                  width: '50px',
                  height: '50px',
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'center',
                  cursor: 'pointer',
                  zIndex: 30,
                  transition: 'background 0.2s',
                }}
                onMouseEnter={(e) => e.currentTarget.style.background = 'rgba(0,0,0,0.8)'}
                onMouseLeave={(e) => e.currentTarget.style.background = 'rgba(0,0,0,0.5)'}
                title="Next Image (Right Arrow)"
              >
                <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><polyline points="9 18 15 12 9 6"></polyline></svg>
              </button>
            </>
          )}
        </div>
        
        {details && (
          <div className="details-overlay" style={{ position: 'absolute', top: 10, left: 10, zIndex: 20 }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
              <button 
                className="details-toggle-btn" 
                onClick={() => setShowDetails(!showDetails)}
                title={showDetails ? "Hide metadata (I)" : "Show metadata (I)"}
                style={{ background: 'rgba(0,0,0,0.5)', color: 'white', border: 'none', padding: '5px 10px', borderRadius: '4px', cursor: 'pointer' }}
              >
                {showDetails ? '▼ Hide Info' : '▲ Show Info'}
              </button>
              {isFetchingNative && (
                <div title="Loading Native Resolution..." style={{ display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
                  <div className="thumbnail-loading-spinner" style={{ width: 16, height: 16, borderWidth: 2 }}></div>
                </div>
              )}
            </div>
            {showDetails && (
              <div className="details-content" style={{ background: 'rgba(30, 41, 59, 0.9)', padding: '10px', marginTop: '5px', borderRadius: '4px', color: 'white', maxWidth: '300px', wordBreak: 'break-all' }}>
                {details}
              </div>
            )}
          </div>
        )}
      </div>
      
      <div className="image-preview-toolbar-container" style={{ position: 'absolute', bottom: 30, left: '50%', transform: 'translateX(-50%)', zIndex: 20 }}>
        <div className="image-preview-toolbar" style={{ display: 'flex', gap: '6px', alignItems: 'center', background: 'rgba(15, 23, 42, 0.8)', padding: '8px 12px', borderRadius: '40px', backdropFilter: 'blur(16px)', border: '1px solid rgba(255, 255, 255, 0.15)', boxShadow: '0 10px 40px rgba(0, 0, 0, 0.4)' }}>
          <IconButton onClick={() => setScale(s => Math.max(s / 1.5, 0.1))} title="Zoom Out (-)">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><circle cx="11" cy="11" r="8"></circle><line x1="21" y1="21" x2="16.65" y2="16.65"></line><line x1="8" y1="11" x2="14" y2="11"></line></svg>
          </IconButton>
          <IconButton onClick={() => { setScale(1); setOffset({x:0, y:0}); }} title="Reset Zoom (0)">
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><polyline points="15 3 21 3 21 9"></polyline><polyline points="9 21 3 21 3 15"></polyline><line x1="21" y1="3" x2="14" y2="10"></line><line x1="3" y1="21" x2="10" y2="14"></line></svg>
          </IconButton>
          <IconButton onClick={() => setScale(s => Math.min(s * 1.5, 10))} title="Zoom In (+)">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><circle cx="11" cy="11" r="8"></circle><line x1="21" y1="21" x2="16.65" y2="16.65"></line><line x1="11" y1="8" x2="11" y2="14"></line><line x1="8" y1="11" x2="14" y2="11"></line></svg>
          </IconButton>
          <div className="toolbar-divider" style={{ width: '1px', height: '20px', background: 'rgba(255, 255, 255, 0.2)', margin: '0 4px' }}></div>
          <IconButton onClick={() => setRotation(r => r - 90)} title="Rotate Left ([)">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><polyline points="1 4 1 10 7 10"></polyline><path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10"></path></svg>
          </IconButton>
          <IconButton onClick={() => setRotation(r => r + 90)} title="Rotate Right (])">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><polyline points="23 4 23 10 17 10"></polyline><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"></path></svg>
          </IconButton>
          <div className="toolbar-divider" style={{ width: '1px', height: '20px', background: 'rgba(255, 255, 255, 0.2)', margin: '0 4px' }}></div>
          {groupImages.length > 0 && !isRepresentative && (
            <>
              {isEjected ? (
                 <IconButton onClick={handleRestore} title="Restore Image (Delete/Backspace)">
                   <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#22c55e" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                     <path d="M3 15v4c0 1.1.9 2 2 2h14a2 2 0 0 0 2-2v-4M12 3v12M8 11l4 4 4-4"/>
                   </svg>
                 </IconButton>
              ) : (
                 <IconButton onClick={handleEject} title="Eject Image (Delete/Backspace)">
                   <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#ef4444" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                     <path d="M3 15v4c0 1.1.9 2 2 2h14a2 2 0 0 0 2-2v-4M12 15V3l-4 4m4-4l4 4"/>
                   </svg>
                 </IconButton>
              )}
              <div className="toolbar-divider" style={{ width: '1px', height: '20px', background: 'rgba(255, 255, 255, 0.2)', margin: '0 4px' }}></div>
            </>
          )}
          <IconButton onClick={copyPath} title="Copy Path (C)">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"></rect><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"></path></svg>
          </IconButton>
        </div>
      </div>
      
      {/* Representative Image Border Overlay */}
      {currentIndex === 0 && !isLoading && (
        <div style={{
          position: 'fixed',
          top: 0,
          left: 0,
          right: 0,
          bottom: 0,
          border: '4px solid rgba(255, 140, 0, 0.7)',
          pointerEvents: 'none',
          zIndex: 9999,
          boxSizing: 'border-box'
        }} />
      )}
    </div>
  );
};

export default ImagePopup;
