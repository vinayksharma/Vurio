import React, { useState, useEffect, useRef, useCallback } from 'react';
import { fetchDuplicateGroups } from '../utils/api';
import { useServerStatusStore, useUIStore } from '../stores';
import type { DuplicateGroup } from '../types';
import DuplicateGroupControl from './DuplicateGroupControl';
import { remapEventEmitter } from '../utils/remapEvents';
import { getActiveRequestCount } from '../utils/thumbnailRequests';
import { errorLogger } from '../utils/errorLogger';
import { thumbnailQueue } from '../utils/thumbnailQueue';

interface DuplicatesPanelProps {
  isActive: boolean;
}

interface ScrollTestStats {
  startTime: number;
  endTime?: number;
  steps: number;
  errors: Array<{ timestamp: number; message: string; type: string }>;
  queueStats: Array<{ step: number; pending: number; active: number; completed: number }>;
  memoryStats: Array<{ step: number; used: number; total: number }>;
  crashes: number;
  totalGroups?: number;
  initialErrorCount?: number;
  lastScrollPosition?: number;
}

const STATUS_REFRESH_COOLDOWN_MS = 2500;

const DuplicatesPanel: React.FC<DuplicatesPanelProps> = ({ isActive }) => {
  const { isOnline, status } = useServerStatusStore();
  const serverDupeCount = status?.duplicate_groups || 0;
  const serverDuplicatesFound = status?.duplicates_found ?? -1; // Total files in groups; -1 = unknown
  const setActivePanel = useUIStore((state) => state.setActivePanel);
  const debugOverlayEnabled = useUIStore((state) => state.debugOverlayEnabled);
  const [totalGroups, setTotalGroups] = useState(0);
  const [isLoading, setIsLoading] = useState(false);
  const [visibleRange, setVisibleRange] = useState({ start: 0, end: 20 });
  const [cacheVersion, setCacheVersion] = useState(0); // Force re-render when cache updates
  const cardStackRef = useRef<HTMLDivElement>(null);

  // Use a Map for O(1) lookup
  const groupsCacheRef = useRef<Map<number, DuplicateGroup>>(new Map());
  const isLoadingRef = useRef(false);
  const scrollRAFRef = useRef<number | null>(null);
  const cleanupTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const loadTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const totalGroupsRef = useRef(0);
  const checkAndLoadMissingRef = useRef<() => void>(() => { });
  const lastTotalGroupsRef = useRef(0);
  const loadGroupsFromAPIRef = useRef<typeof loadGroupsFromAPI | null>(null);
  const isInitializedRef = useRef(false);
  const lastRemapTimestampRef = useRef<number>(0);
  const [thumbnailReloadKey, setThumbnailReloadKey] = useState<number>(0);
  const memoryCleanupIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const lastMemoryCleanupRef = useRef<number>(0);
  const scrollStopTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [scrollPercent, setScrollPercent] = useState<number>(0);
  const [activeThumbnailRequests, setActiveThumbnailRequests] = useState<number>(0);
  const [errorCount, setErrorCount] = useState<number>(0);
  const [queueStats, setQueueStats] = useState<{ pending: number; active: number; completed: number }>({ pending: 0, active: 0, completed: 0 });
  const scrollTestRunningRef = useRef<boolean>(false);
  const scrollTestStatsRef = useRef<ScrollTestStats | null>(null);
  const lastStatusDrivenRefreshRef = useRef<number>(0);
  const lastSyncedDuplicatesFoundRef = useRef<number>(-1);

  const SCROLL_TEST_STORAGE_KEY = 'dedup_scroll_test_state';

  const CARD_HEIGHT = 150; // Increased from 140 to accommodate taller key thumbnails
  const GAP = 12;
  const ITEM_HEIGHT = CARD_HEIGHT + GAP;
  const BATCH_SIZE = 30;
  const BUFFER = 5;
  const CACHE_SIZE = 300;

  // Keep totalGroups in sync with ref for use in callbacks
  useEffect(() => {
    totalGroupsRef.current = totalGroups;
  }, [totalGroups]);

  // Update active thumbnail request count, error count, and queue stats for debug panel
  useEffect(() => {
    const updateCounts = () => {
      setActiveThumbnailRequests(getActiveRequestCount());
      setErrorCount(errorLogger.getErrorCount());
      const stats = thumbnailQueue.getStats();
      setQueueStats({ pending: stats.pending, active: stats.active, completed: stats.completed });
    };

    updateCounts(); // Update immediately on mount
    const ms = debugOverlayEnabled && isActive ? 100 : 5000; // 100ms when debugging, 5s when not
    const interval = setInterval(updateCounts, ms);
    return () => clearInterval(interval);
  }, [debugOverlayEnabled, isActive]);

  // Calculate the visible range based on scroll position
  const calculateVisibleRange = useCallback((container: HTMLElement) => {
    const scrollTop = container.scrollTop;
    const containerHeight = container.clientHeight;
    const total = totalGroupsRef.current;

    if (total === 0) return { start: 0, end: 20 };

    const startIndex = Math.max(0, Math.floor(scrollTop / ITEM_HEIGHT) - BUFFER);
    const visibleCount = Math.ceil(containerHeight / ITEM_HEIGHT);
    const endIndex = Math.min(total - 1, Math.floor(scrollTop / ITEM_HEIGHT) + visibleCount + BUFFER);

    return { start: startIndex, end: Math.max(startIndex, endIndex) };
  }, []);

  // Load groups from API - simple, no recursion
  const loadGroupsFromAPI = useCallback(async (startIndex: number, count: number): Promise<boolean> => {
    if (isLoadingRef.current) return false;

    isLoadingRef.current = true;
    setIsLoading(true);

    try {
      console.log(`Fetching groups: start=${startIndex}, limit=${count}`);
      const data = await fetchDuplicateGroups(startIndex, count, null);

      const total = data.total_count ?? data.total ?? 0;
      // Only update if total actually changed to prevent unnecessary re-renders
      if (total !== totalGroupsRef.current && total > 0) {
        setTotalGroups(total);
        totalGroupsRef.current = total;
      }

      if (data.groups && Array.isArray(data.groups)) {
        const cache = groupsCacheRef.current;
        let hasChanges = false;

        // Always overwrite cache with fresh data so membership changes are reflected
        data.groups.forEach((group, idx) => {
          const key = startIndex + idx;
          if (!cache.has(key)) hasChanges = true;
          cache.set(key, group);
        });

        if (hasChanges || data.groups.length > 0) {
          setCacheVersion(v => v + 1);
        }
        return true;
      }
      return false;
    } catch (error) {
      console.error('Failed to load groups:', error);
      return false;
    } finally {
      isLoadingRef.current = false;
      setIsLoading(false);
    }
  }, []);

  // Keep ref in sync
  useEffect(() => {
    loadGroupsFromAPIRef.current = loadGroupsFromAPI;
  }, [loadGroupsFromAPI]);

  // Check and load missing groups in visible range
  const checkAndLoadMissing = useCallback(() => {
    if (isLoadingRef.current || !cardStackRef.current || !isActive || !isOnline) return;

    const container = cardStackRef.current;
    if (!container) return;

    try {
      const range = calculateVisibleRange(container);
      const cache = groupsCacheRef.current;
      const total = totalGroupsRef.current;

      if (total === 0) {
        if (cache.size === 0 && !isLoadingRef.current && loadGroupsFromAPIRef.current) {
          loadGroupsFromAPIRef.current(0, BATCH_SIZE).catch(err => {
            console.error('Error loading initial groups:', err);
          });
        }
        return;
      }

      let firstMissing = -1;
      let missingCount = 0;
      const safeEnd = Math.min(range.end, total - 1);

      for (let i = range.start; i <= safeEnd; i++) {
        if (!cache.has(i)) {
          if (firstMissing === -1) firstMissing = i;
          missingCount++;
        }
      }

      if (firstMissing >= 0 && missingCount > 0) {
        const batchSize = Math.min(missingCount, BATCH_SIZE);
        loadGroupsFromAPI(firstMissing, batchSize).catch(err => {
          console.error('Error in checkAndLoadMissing:', err);
        });
      }
    } catch (error) {
      console.error('Error in checkAndLoadMissing:', error);
    }
  }, [calculateVisibleRange, loadGroupsFromAPI, isActive, isOnline]);

  useEffect(() => {
    checkAndLoadMissingRef.current = checkAndLoadMissing;
  }, [checkAndLoadMissing]);

  // Prune cache to keep only items near the viewport
  const pruneCache = useCallback((currentStart: number, currentEnd: number) => {
    const cache = groupsCacheRef.current;
    if (cache.size <= CACHE_SIZE) return;

    const keepStart = Math.max(0, currentStart - CACHE_SIZE / 2);
    const keepEnd = currentEnd + CACHE_SIZE / 2;

    for (const key of Array.from(cache.keys())) {
      if (key < keepStart || key > keepEnd) {
        cache.delete(key);
      }
    }
  }, []);

  // Refresh visible range without changing scroll
  const refreshVisibleRange = useCallback(() => {
    if (!cardStackRef.current || !isActive || !isOnline || isLoadingRef.current) return;
    const container = cardStackRef.current;
    const range = calculateVisibleRange(container);
    const rangeSize = range.end - range.start + 1;
    const count = Math.min(BATCH_SIZE * 2, Math.max(1, rangeSize));
    loadGroupsFromAPI(range.start, count);
  }, [calculateVisibleRange, loadGroupsFromAPI, isActive, isOnline]);

  // Handle scroll events
  const handleScroll = useCallback(() => {
    if (!cardStackRef.current) return;

    if (scrollRAFRef.current) {
      cancelAnimationFrame(scrollRAFRef.current);
    }

    scrollRAFRef.current = requestAnimationFrame(() => {
      const container = cardStackRef.current;
      if (!container) return;

      const newRange = calculateVisibleRange(container);

      const scrollTop = container.scrollTop;
      const scrollHeight = container.scrollHeight - container.clientHeight;
      const percent = scrollHeight > 0 ? Math.round((scrollTop / scrollHeight) * 100) : 0;
      setScrollPercent(percent);

      setVisibleRange(prev => {
        if (prev.start === newRange.start && prev.end === newRange.end) {
          return prev;
        }
        return newRange;
      });

      if (lastRemapTimestampRef.current > 0 && Date.now() - lastRemapTimestampRef.current < 30000) {
        setThumbnailReloadKey(prev => prev + 1);
        setCacheVersion(v => v + 1);
      }

      if (loadTimeoutRef.current) {
        clearTimeout(loadTimeoutRef.current);
      }
      loadTimeoutRef.current = setTimeout(() => {
        checkAndLoadMissing();
      }, 100);

      if (cleanupTimeoutRef.current) {
        clearTimeout(cleanupTimeoutRef.current);
      }
      cleanupTimeoutRef.current = setTimeout(() => {
        pruneCache(newRange.start, newRange.end);
      }, 2000);

      scrollRAFRef.current = null;
    });
  }, [calculateVisibleRange, checkAndLoadMissing, pruneCache]);

  useEffect(() => {
    window.startScrollTest = async () => {}; // Stub for build
    window.stopScrollTest = () => {}; // Stub for build
    return () => {
      delete (window as any).startScrollTest;
      delete (window as any).stopScrollTest;
    };
  }, []);

  // Initialize when panel becomes active
  useEffect(() => {
    if (!isActive || !isOnline) {
      groupsCacheRef.current.clear();
      setTotalGroups(0);
      totalGroupsRef.current = 0;
      lastTotalGroupsRef.current = 0;
      lastSyncedDuplicatesFoundRef.current = -1;
      setVisibleRange({ start: 0, end: 20 });
      setCacheVersion(0);
      isInitializedRef.current = false;
      return;
    }

    if (!isInitializedRef.current && groupsCacheRef.current.size === 0 && !isLoadingRef.current && loadGroupsFromAPIRef.current) {
      isInitializedRef.current = true;
      loadGroupsFromAPIRef.current(0, BATCH_SIZE).catch(err => {
        console.error('Error during initialization:', err);
        isInitializedRef.current = false;
      });
    }
  }, [isActive, isOnline]);

  useEffect(() => {
    const container = cardStackRef.current;
    if (!container || !isActive) return;

    container.addEventListener('scroll', handleScroll, { passive: true });

    return () => {
      container.removeEventListener('scroll', handleScroll);
      if (scrollRAFRef.current) {
        cancelAnimationFrame(scrollRAFRef.current);
      }
      if (cleanupTimeoutRef.current) {
        clearTimeout(cleanupTimeoutRef.current);
      }
      if (loadTimeoutRef.current) {
        clearTimeout(loadTimeoutRef.current);
      }
    };
  }, [isActive, handleScroll]);

  useEffect(() => {
    if (!isLoading && isActive && totalGroups > 0) {
      if (totalGroups !== lastTotalGroupsRef.current) {
        lastTotalGroupsRef.current = totalGroups;
        const timeoutId = setTimeout(() => {
          checkAndLoadMissingRef.current();
        }, 200);
        return () => clearTimeout(timeoutId);
      }
    }
  }, [isLoading, isActive, totalGroups]);

  useEffect(() => {
    if (!isActive || !isOnline) return;
    if (serverDupeCount === 0 && totalGroups === 0 && serverDuplicatesFound <= 0) return;

    const groupsOutOfSync = serverDupeCount !== totalGroups;
    const membersOutOfSync = serverDuplicatesFound >= 0 && serverDuplicatesFound !== lastSyncedDuplicatesFoundRef.current;
    if (!groupsOutOfSync && !membersOutOfSync) return;

    const now = Date.now();
    if (now - lastStatusDrivenRefreshRef.current < STATUS_REFRESH_COOLDOWN_MS) return;

    lastStatusDrivenRefreshRef.current = now;
    lastSyncedDuplicatesFoundRef.current = serverDuplicatesFound;
    refreshVisibleRange();
  }, [isActive, isOnline, serverDupeCount, serverDuplicatesFound, totalGroups, refreshVisibleRange]);

  const totalHeight = totalGroups > 0 ? totalGroups * ITEM_HEIGHT - GAP : 200;

  const renderItems = useCallback(() => {
    if (totalGroups === 0) {
      if (isOnline && !isLoading) {
        return (
          <div className="card placeholder duplicates-empty-state" style={{ position: 'relative', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 16, padding: 32 }}>
            <p style={{ margin: 0, color: 'var(--muted)', textAlign: 'center' }}>
              No duplicates found yet. Register a media location to start scanning.
            </p>
            <button className="primary" onClick={() => setActivePanel('settings')} style={{ minWidth: 180 }}>
              Add Media Location
            </button>
          </div>
        );
      }
      return <div className="card placeholder" style={{ position: 'relative' }}>{isLoading ? 'Loading...' : 'Duplicate cards will appear here…'}</div>;
    }

    const cache = groupsCacheRef.current;
    const items: React.ReactNode[] = [];

    for (let idx = visibleRange.start; idx <= visibleRange.end && idx < totalGroups; idx++) {
      const top = idx * ITEM_HEIGHT;
      const group = cache.get(idx);

      if (group) {
        items.push(
          <div key={`group-${idx}`} data-group-index={idx} style={{ position: 'absolute', top: `${top}px`, left: 0, right: 0, height: `${CARD_HEIGHT}px`, overflow: 'hidden' }}>
            <DuplicateGroupControl group={group} />
          </div>
        );
      } else {
        items.push(
          <div key={`placeholder-${idx}`} className="card placeholder" style={{ position: 'absolute', top: `${top}px`, left: 0, right: 0, height: `${CARD_HEIGHT}px`, display: 'flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden' }}>
            {isLoading ? 'Loading...' : `Group ${idx + 1}`}
          </div>
        );
      }
    }
    return items;
  }, [visibleRange, totalGroups, isLoading, isOnline, cacheVersion, thumbnailReloadKey, setActivePanel]);

  if (!isOnline) {
    return (
      <section id="panel-duplicates" className={`content-panel ${isActive ? 'active' : ''}`}>
        <header className="panel-header">
          <div>
            <h1>Duplicate Groups</h1>
            <p className="panel-description">Review similar media files grouped by similarity and take action directly from the list.</p>
          </div>
        </header>
        <section className="panel duplicates-panel">
          <div className="card-stack" id="duplicates-stack">
            <div className="card placeholder">Server offline - thumbnails unavailable</div>
          </div>
        </section>
      </section>
    );
  }

  return (
    <section id="panel-duplicates" className={`content-panel ${isActive ? 'active' : ''}`}>
      <header className="panel-header">
        <div>
          <h1>Duplicate Groups</h1>
          <p className="panel-description">Review similar media files grouped by similarity and take action directly from the list.</p>
        </div>
      </header>
      <section className="panel duplicates-panel">
        <div className="panel-title split">
          <span>Live Duplicate Feed</span>
          <span className="panel-meta" id="duplicates-meta">{totalGroups > 0 ? `${totalGroups} groups` : 'No duplicates'}</span>
        </div>
        <div className="card-stack" id="duplicates-stack" ref={cardStackRef} style={{ position: 'relative', overflow: 'auto' }}>
          <div style={{ height: `${totalHeight}px`, position: 'relative' }}>{renderItems()}</div>
        </div>
      </section>
    </section>
  );
};

export default DuplicatesPanel;
