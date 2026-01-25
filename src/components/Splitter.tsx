import { useState, useRef, useEffect, useCallback } from 'react';
import './Splitter.css';

interface SplitterProps {
  direction: 'horizontal' | 'vertical';
  onResize: (size: number) => void;
  initialSize?: number;
  minSize?: number;
  maxSize?: number;
}

const Splitter: React.FC<SplitterProps> = ({
  direction,
  onResize,
  initialSize,
  minSize = 100,
  maxSize,
}) => {
  const [isDragging, setIsDragging] = useState(false);
  const [size, setSize] = useState(initialSize || 260);
  const splitterRef = useRef<HTMLDivElement>(null);
  const startPosRef = useRef<number>(0);
  const startSizeRef = useRef<number>(0);

  // Update size when initialSize prop changes
  useEffect(() => {
    if (initialSize !== undefined && !isDragging) {
      setSize(initialSize);
    }
  }, [initialSize, isDragging]);

  const handleMouseDown = useCallback((e: React.MouseEvent) => {
    e.preventDefault();
    setIsDragging(true);
    if (direction === 'horizontal') {
      startPosRef.current = e.clientX;
    } else {
      startPosRef.current = e.clientY;
    }
    startSizeRef.current = size;
  }, [direction, size]);

  useEffect(() => {
    if (!isDragging) return;

    const handleMouseMove = (e: MouseEvent) => {
      const delta = direction === 'horizontal'
        ? e.clientX - startPosRef.current
        : e.clientY - startPosRef.current;
      
      let newSize = startSizeRef.current + delta;
      newSize = Math.max(minSize, newSize);
      if (maxSize !== undefined) {
        newSize = Math.min(maxSize, newSize);
      }
      
      setSize(newSize);
      onResize(newSize);
    };

    const handleMouseUp = () => {
      setIsDragging(false);
    };

    document.addEventListener('mousemove', handleMouseMove);
    document.addEventListener('mouseup', handleMouseUp);

    return () => {
      document.removeEventListener('mousemove', handleMouseMove);
      document.removeEventListener('mouseup', handleMouseUp);
    };
  }, [isDragging, direction, minSize, maxSize, onResize]);

  return (
    <div
      ref={splitterRef}
      className={`splitter splitter-${direction} ${isDragging ? 'dragging' : ''}`}
      onMouseDown={handleMouseDown}
    >
      <div className="splitter-handle" />
    </div>
  );
};

export default Splitter;
