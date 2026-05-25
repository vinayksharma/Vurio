import React, { useState, useEffect, useRef, useCallback } from 'react';
import { fetchDuplicateGroups, fetchUserSetting, saveUserSetting, fetchUserSettings, ejectDuplicates, bulkUndoEjectDuplicates } from '../utils/api';
import { useServerStatusStore, useUIStore } from '../stores';
import type { DuplicateGroup, DuplicateGroupMember } from '../types';
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
  const isOnline = useServerStatusStore((state) => state.isOnline);
  const serverDupeCount = useServerStatusStore((state) => state.status?.duplicate_groups || 0);
  const serverDuplicatesFound = useServerStatusStore((state) => state.status?.duplicates_found ?? -1); // Total files in groups; -1 = unknown

  const hasEjectedFiles = useServerStatusStore((state) => (state.status as any)?.has_ejected_files || false);
  const registeredDirsCount = useServerStatusStore((state) => state.registeredDirs.length);
  const setActivePanel = useUIStore((state) => state.setActivePanel);
  const debugOverlayEnabled = useUIStore((state) => state.debugOverlayEnabled);
  const duplicateTightness = useUIStore((state) => state.duplicateTightness);
  const setDuplicateTightness = useUIStore((state) => state.setDuplicateTightness);
  const [totalGroups, setTotalGroups] = useState(0);
  const [isLoading, setIsLoading] = useState(false);
  const [visibleRange, setVisibleRange] = useState({ start: 0, end: 20 });
  const visibleRangeRef = useRef(visibleRange);
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
  const refreshVisibleRangeRef = useRef<() => void>(() => {});
  const [thumbnailReloadKey, setThumbnailReloadKey] = useState<number>(0);
  const scrollStopTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [scrollPercent, setScrollPercent] = useState<number>(0);
  const [activeThumbnailRequests, setActiveThumbnailRequests] = useState<number>(0);
  const [errorCount, setErrorCount] = useState<number>(0);
  const [queueStats, setQueueStats] = useState<{ pending: number; active: number; completed: number }>({ pending: 0, active: 0, completed: 0 });
  const scrollTestRunningRef = useRef<boolean>(false);
  const scrollTestStatsRef = useRef<ScrollTestStats | null>(null);
  const lastStatusDrivenRefreshRef = useRef<number>(0);
  const lastSyncedDuplicatesFoundRef = useRef<number>(-1);
  const [bulkEjectState, setBulkEjectState] = useState<{ type: 'eject' | 'restore'; calculating: boolean; current: number; total: number; } | null>(null);

  const isDraggingRef = useRef(false);
  const dragStartYRef = useRef(0);
  const dragStartScrollTopRef = useRef(0);

  const SCROLL_TEST_STORAGE_KEY = 'dedup_scroll_test_state';

  const CARD_HEIGHT = 166; // Increased from 150 to accommodate taller key thumbnails
  const GAP = 12;
  const ITEM_HEIGHT = CARD_HEIGHT + GAP;
  const BATCH_SIZE = 30;
  const BUFFER = 5;
  const CACHE_SIZE = 300;

  // Keep totalGroups in sync with ref for use in callbacks
  useEffect(() => {
    totalGroupsRef.current = totalGroups;
  }, [totalGroups]);

  useEffect(() => {
    visibleRangeRef.current = visibleRange;
  }, [visibleRange]);

  // Periodic polling of the visible range to catch inner group updates
  useEffect(() => {
    if (!isActive || !isOnline) return;

    const fetchVisible = () => {
      refreshVisibleRangeRef.current();
    };

    // Immediate fetch to refresh stale cache when returning to the view
    const delayId = setTimeout(fetchVisible, 100);
    const intervalId = setInterval(fetchVisible, 5000);

    return () => {
      clearTimeout(delayId);
      clearInterval(intervalId);
    };
  }, [isActive, isOnline]);

  useEffect(() => {
    let mounted = true;
    fetchUserSetting('duplicate_tightness').then(val => {
      if (!mounted) return;
      if (val !== null) {
        const parsed = parseFloat(val);
        if (!isNaN(parsed) && parsed >= 0 && parsed <= 1) {
          setDuplicateTightness(parsed);
        }
      }
    });
    return () => { mounted = false; };
  }, [setDuplicateTightness]);

  const [sliderValue, setSliderValue] = useState<number>(0);
  
  useEffect(() => {
    setSliderValue(Math.round(Math.cbrt(duplicateTightness) * 1000));
  }, [duplicateTightness]);

  const handleSliderChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    const v = parseInt(e.target.value, 10);
    setSliderValue(v);
    const t = Math.pow(v / 1000, 3);
    setDuplicateTightness(t);
  };

  const handleSliderPointerUp = () => {
    const t = Math.pow(sliderValue / 1000, 3);
    saveUserSetting('duplicate_tightness', t.toString()).catch(e => console.error('Failed to save lightness:', e));
  };

  // Update active thumbnail request count, error count, and queue stats for debug panel
  useEffect(() => {
    const updateCounts = () => {
      setActiveThumbnailRequests(getActiveRequestCount());
      setErrorCount(errorLogger.getErrorCount());
      const stats = thumbnailQueue.getStats();
      setQueueStats(prev => {
        if (prev.pending === stats.pending && prev.active === stats.active && prev.completed === stats.completed) return prev;
        return { pending: stats.pending, active: stats.active, completed: stats.completed };
      });
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

  // Refresh visible range without changing scroll, using Scroll Anchoring to prevent jumps
  const refreshVisibleRange = useCallback(async () => {
    if (!cardStackRef.current || !isActive || !isOnline || isLoadingRef.current) return;
    const container = cardStackRef.current;
    const range = calculateVisibleRange(container);
    const rangeSize = range.end - range.start + 1;
    
    // Fetch a wider window to find the anchor if it shifted
    const fetchStart = Math.max(0, range.start - 20);
    const fetchCount = rangeSize + 40;
    
    // Identify anchor
    const anchorIndex = range.start;
    const anchorGroup = groupsCacheRef.current.get(anchorIndex);
    const anchorId = anchorGroup?.id;
    const anchorRep = anchorGroup?.representative;
    
    isLoadingRef.current = true;
    try {
      const data = await fetchDuplicateGroups(fetchStart, fetchCount, null);
      
      const total = data.total_count ?? data.total ?? 0;
      if (total !== totalGroupsRef.current && total > 0) {
        setTotalGroups(total);
        totalGroupsRef.current = total;
      }
      
      if (!data.groups || !Array.isArray(data.groups)) return;
      
      let delta = 0;
      if (anchorId !== undefined || anchorRep !== undefined) {
        let newIndex = -1;
        for (let i = 0; i < data.groups.length; i++) {
          const g = data.groups[i];
          if ((anchorId !== undefined && g.id === anchorId) || 
              (anchorRep !== undefined && g.representative === anchorRep)) {
            newIndex = fetchStart + i;
            break;
          }
        }
        
        if (newIndex !== -1 && newIndex !== anchorIndex) {
          delta = newIndex - anchorIndex;
          console.log(`[Anchoring] Group shifted from ${anchorIndex} to ${newIndex}. Delta: ${delta}`);
          
          const newCache = new Map<number, DuplicateGroup>();
          for (const [k, v] of groupsCacheRef.current.entries()) {
            newCache.set(k + delta, v);
          }
          groupsCacheRef.current = newCache;
          
          container.scrollTop += delta * ITEM_HEIGHT;
          setVisibleRange(calculateVisibleRange(container));
        }
      }
      
      let hasChanges = false;
      data.groups.forEach((group, idx) => {
        const key = fetchStart + idx;
        if (!groupsCacheRef.current.has(key)) hasChanges = true;
        groupsCacheRef.current.set(key, group);
      });
      
      if (hasChanges || data.groups.length > 0 || delta !== 0) {
        setCacheVersion(v => v + 1);
      }
    } catch (e) {
      console.error('Anchored fetch failed:', e);
    } finally {
      isLoadingRef.current = false;
    }
  }, [calculateVisibleRange, isActive, isOnline, ITEM_HEIGHT]);

  useEffect(() => {
    refreshVisibleRangeRef.current = refreshVisibleRange;
  }, [refreshVisibleRange]);

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
      const percent = scrollHeight > 0 ? (scrollTop / scrollHeight) * 100 : 0;
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
      if (scrollStopTimeoutRef.current) {
        clearTimeout(scrollStopTimeoutRef.current);
        scrollStopTimeoutRef.current = null;
      }
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

  // Listen to forced cache reset signals (e.g. from AdvancedSection's integrity check)
  useEffect(() => {
    const handleForceRefresh = () => {
      console.log('[DuplicatesPanel] Force refresh requested, clearing cache');
      groupsCacheRef.current.clear();
      setCacheVersion(v => v + 1);
      lastStatusDrivenRefreshRef.current = 0; // reset cooldown
      refreshVisibleRangeRef.current();
    };

    window.addEventListener('duplicatesForceRefresh', handleForceRefresh);
    return () => window.removeEventListener('duplicatesForceRefresh', handleForceRefresh);
  }, []);

  useEffect(() => {
    const container = cardStackRef.current;
    if (container) {
      container.addEventListener('scroll', handleScroll, { passive: true });
    }

    return () => {
      if (container) {
        container.removeEventListener('scroll', handleScroll);
      }
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

  useEffect(() => {
    const bc = new BroadcastChannel('viewer_group_navigation');
    bc.onmessage = async (e) => {
      if (e.data.type === 'NAVIGATE_GROUP') {
        const { direction, jumpType, currentFilePath } = e.data;
        const cache = groupsCacheRef.current;
        let currentGroupIndex = -1;
        
        for (const [idx, group] of cache.entries()) {
           if (group.members?.some(m => m.file_path === currentFilePath) || group.representative === currentFilePath || group.candidates?.includes(currentFilePath)) {
              currentGroupIndex = idx;
              break;
           }
        }
        
        if (currentGroupIndex !== -1 && totalGroupsRef.current > 0) {
           let jumpAmount = direction;
           if (jumpType === 'page' && cardStackRef.current) {
             const visibleCount = Math.max(1, Math.floor(cardStackRef.current.clientHeight / ITEM_HEIGHT) - 1);
             jumpAmount = direction * visibleCount;
           }
           
           let targetIndex = currentGroupIndex + jumpAmount;
           
           if (jumpType === 'page') {
             if (targetIndex < 0) targetIndex = 0;
             if (targetIndex >= totalGroupsRef.current) targetIndex = totalGroupsRef.current - 1;
           } else {
             if (targetIndex < 0) targetIndex = totalGroupsRef.current - 1;
             if (targetIndex >= totalGroupsRef.current) targetIndex = 0;
           }
           
           let targetGroup = cache.get(targetIndex);
           if (!targetGroup && loadGroupsFromAPIRef.current) {
              await loadGroupsFromAPIRef.current(targetIndex, 1);
              targetGroup = groupsCacheRef.current.get(targetIndex);
           }
           
           if (targetGroup) {
              useUIStore.getState().setActiveViewerGroupId(targetGroup.id);
              let sortedMembers: DuplicateGroupMember[] = [];
              if (targetGroup.members && targetGroup.members.length > 0) {
                sortedMembers = [...targetGroup.members].sort((a, b) => {
                  const aIsRep = a.file_path === targetGroup!.representative || a.is_representative;
                  const bIsRep = b.file_path === targetGroup!.representative || b.is_representative;
                  if (aIsRep && !bIsRep) return -1;
                  if (!aIsRep && bIsRep) return 1;
                  return (b.similarity_score || 0) - (a.similarity_score || 0);
                });
                const repInMembers = sortedMembers.some(m => m.file_path === targetGroup!.representative);
                if (!repInMembers) {
                  sortedMembers.unshift({ file_path: targetGroup!.representative, similarity_score: 1.0, is_representative: true, file_size: 0 } as DuplicateGroupMember);
                }
              } else {
                sortedMembers = [
                  { file_path: targetGroup.representative, similarity_score: 1.0, is_representative: true, file_size: 0 } as DuplicateGroupMember,
                  ...(targetGroup.candidates || []).map(path => ({ file_path: path, similarity_score: targetGroup!.similarity_threshold || 0.9, file_size: 0 } as DuplicateGroupMember))
                ];
              }
              
              const displayId = targetIndex + 1;
              const groupImages = sortedMembers.map((member, index) => {
                const isFirst = index === 0;
                const rankText = isFirst ? 'Highest Similarity' : `Rank #${index + 1}`;
                const itemTooltip = `Group #${displayId} - ${rankText}\nPath: ${member.file_path}\nSimilarity: ${member.similarity_score != null ? member.similarity_score.toFixed(4) : 'N/A'}\nFile Size: ${member.file_size ? (member.file_size / 1024 / 1024).toFixed(1) + 'MB' : 'Unknown'}\nDate Taken: ${member.created_date ? new Date(member.created_date).toLocaleString() : 'Unknown'}\nAdded: ${member.added_at ? new Date(member.added_at).toLocaleString() : 'Unknown'}\nRepresentative: ${member.is_representative ? 'Yes' : 'No'}`;
                return { filePath: member.file_path, details: itemTooltip, is_ejected: member.is_ejected };
              });
              
              const container = cardStackRef.current;
              if (container) {
                const targetScroll = targetIndex * ITEM_HEIGHT;
                container.scrollTo({ top: targetScroll, behavior: 'smooth' });
              }
              
              bc.postMessage({
                 type: 'NEW_GROUP',
                 groupImages,
                 currentIndex: 0,
                 filePath: groupImages[0].filePath,
                 details: groupImages[0].details
              });
           }
        }
      } else if (e.data.type === 'VIEWER_CLOSED') {
        useUIStore.getState().setActiveViewerGroupId(null);
      }
    };
    return () => bc.close();
  }, [ITEM_HEIGHT]);

  const totalHeight = totalGroups > 0 ? totalGroups * ITEM_HEIGHT - GAP : 200;

  const renderItems = useCallback(() => {
    if (totalGroups === 0) {
      if (isOnline && !isLoading) {
        return (
          <div className="card placeholder duplicates-empty-state" style={{ position: 'relative', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 16, padding: 32 }}>
            <p style={{ margin: 0, color: 'var(--muted)', textAlign: 'center' }}>
              {registeredDirsCount === 0 
                ? 'No duplicates found yet. Register a media location to start scanning.' 
                : 'No duplicates found yet. Check back once scanning is complete, or adjust your duplication selection scale.'}
            </p>
            {registeredDirsCount === 0 && (
              <button className="primary" onClick={() => setActivePanel('settings')} style={{ minWidth: 180 }}>
                Add Media Location
              </button>
            )}
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
            <DuplicateGroupControl group={group} groupIndex={idx} />
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

  const handleBulkEject = async () => {
    if (bulkEjectState) return;

    setBulkEjectState({ type: 'eject', calculating: true, current: 0, total: 0 });

    const LIMIT = 1000;
    let start = 0;
    let hasMore = true;

    const filePathsToEject: string[] = [];
    const eligibleMembers: { groupIndex: number, memberIndex: number, filePath: string }[] = [];

    // Phase 1: Count
    while (hasMore) {
      try {
        const data = await fetchDuplicateGroups(start, LIMIT, null);
        if (!data || !data.groups || data.groups.length === 0) break;

        data.groups.forEach((group, idx) => {
          const groupIndex = start + idx;
          group.members?.forEach((member, mIdx) => {
            if (member.is_ejected) return;
            if (member.file_path === group.representative || member.is_representative) return;
            
            const score = typeof member.similarity_score === 'number' ? member.similarity_score : 1;
            if ((1.0 - score) <= duplicateTightness) {
              filePathsToEject.push(member.file_path);
              eligibleMembers.push({ groupIndex, memberIndex: mIdx, filePath: member.file_path });
            }
          });
        });

        start += data.returned || data.groups.length;
        if (start >= (data.total_count || 0) || (data.returned || 0) < LIMIT) {
          hasMore = false;
        }
      } catch (error) {
        console.error("Error calculating bulk eject:", error);
        break;
      }
    }

    if (filePathsToEject.length === 0) {
      alert("No eligible duplicates found to eject securely under the current duplication selection scale.");
      setBulkEjectState(null);
      return;
    }

    if (!window.confirm(`Found ${filePathsToEject.length} identical duplicates based on your duplication selection scale. Eject them all?`)) {
      setBulkEjectState(null);
      return;
    }

    // Phase 2: Execute
    setBulkEjectState({ type: 'eject', calculating: false, current: 0, total: filePathsToEject.length });

    (async () => {
      const chunkSize = 50;
      const cache = groupsCacheRef.current;

      for (let i = 0; i < filePathsToEject.length; i += chunkSize) {
        const chunk = filePathsToEject.slice(i, i + chunkSize);

        try {
          await ejectDuplicates(chunk);

          const chunkSet = new Set(chunk);
          let cacheChanged = false;

          eligibleMembers.forEach(m => {
            if (chunkSet.has(m.filePath)) {
              const g = cache.get(m.groupIndex);
              if (g && g.members && g.members[m.memberIndex]) {
                g.members[m.memberIndex].is_ejected = true;
                cacheChanged = true;
              }
            }
          });

          if (cacheChanged) setCacheVersion(v => v + 1);
          setBulkEjectState(prev => prev ? { ...prev, current: Math.min(i + chunk.length, filePathsToEject.length) } : null);
        } catch (error) {
          console.error("Bulk eject chunk failed:", error);
          if (window.errorLogger) {
            window.errorLogger.logError({
              type: 'react',
              message: 'Failed during bulk eject iteration',
              context: { error: String(error) }
            });
          }
        }
      }
      setTimeout(() => setBulkEjectState(null), 2000); // Clear status after 2s
    })();
  };

  const handleBulkUndoEject = async () => {
    if (bulkEjectState) return;

    if (!window.confirm(`Are you sure you want to globally undo all ejections and securely restore all ejected files back to their original locations?`)) {
      return;
    }

    setBulkEjectState({ type: 'restore', calculating: true, current: 0, total: 1 });

    try {
      const result = await bulkUndoEjectDuplicates();
      if (result.success) {
        // Clear cache and trigger refetch
        setCacheVersion(v => v + 1);
        groupsCacheRef.current.clear();
        setBulkEjectState({ type: 'restore', calculating: false, current: 1, total: 1 });
      } else {
        alert("Failed to undo bulk eject: " + result.error);
      }
    } catch (e) {
      alert("Error undoing bulk eject.");
    } finally {
      setTimeout(() => setBulkEjectState(null), 2000);
    }
  };

  const handleThumbPointerDown = (e: React.PointerEvent<HTMLDivElement>) => {
    if (!cardStackRef.current) return;
    e.preventDefault();
    e.stopPropagation();
    isDraggingRef.current = true;
    dragStartYRef.current = e.clientY;
    dragStartScrollTopRef.current = cardStackRef.current.scrollTop;
    document.body.style.userSelect = 'none';
    
    const handlePointerMove = (moveEvent: PointerEvent) => {
      if (!isDraggingRef.current || !cardStackRef.current) return;
      const deltaY = moveEvent.clientY - dragStartYRef.current;
      const trackHeight = cardStackRef.current.clientHeight - 8;
      const thumbHeight = Math.max(20, trackHeight * (trackHeight / Math.max(1, cardStackRef.current.scrollHeight)));
      const scrollableTrackHeight = trackHeight - thumbHeight;
      if (scrollableTrackHeight <= 0) return;
      
      const scrollRatio = deltaY / scrollableTrackHeight;
      const maxScrollTop = cardStackRef.current.scrollHeight - cardStackRef.current.clientHeight;
      
      cardStackRef.current.scrollTop = Math.max(0, Math.min(maxScrollTop, dragStartScrollTopRef.current + (scrollRatio * maxScrollTop)));
    };
    
    const handlePointerUp = () => {
      isDraggingRef.current = false;
      document.body.style.userSelect = '';
      window.removeEventListener('pointermove', handlePointerMove);
      window.removeEventListener('pointerup', handlePointerUp);
    };
    
    window.addEventListener('pointermove', handlePointerMove);
    window.addEventListener('pointerup', handlePointerUp);
  };

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
        <div className="tightness-slider-container">
          <span className="slider-label" style={{ display: 'flex', alignItems: 'center', gap: '4px' }}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
              <rect x="9" y="9" width="13" height="13" rx="2" ry="2"></rect>
              <path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"></path>
            </svg>
            identical
          </span>
          <input 
            type="range" 
            min="0" 
            max="1000" 
            value={sliderValue} 
            onChange={handleSliderChange} 
            onMouseUp={handleSliderPointerUp} 
            onTouchEnd={handleSliderPointerUp}
            className="tightness-slider"
            title="Adjust duplication selection scale for perfect matches"
            style={{ '--slider-value': `${(sliderValue / 1000) * 100}%` } as React.CSSProperties}
          />
          <span className="slider-label right" style={{ display: 'flex', alignItems: 'center', gap: '4px', justifyContent: 'flex-end' }}>
            similar
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
              <circle cx="9" cy="12" r="6" />
              <circle cx="15" cy="12" r="6" />
            </svg>
          </span>
          
          <div style={{ marginLeft: 'auto', display: 'flex', gap: '8px' }}>
            <button 
              className="danger icon-button" 
              onClick={handleBulkEject}
              disabled={!!bulkEjectState}
              title="Eject all duplicates matching the current duplication selection scale"
              style={{
                width: '105px',
                justifyContent: 'center',
                padding: '6px 12px', display: 'flex', alignItems: 'center', gap: '6px', fontSize: '11px', 
                backgroundColor: bulkEjectState ? 'rgba(100, 100, 100, 0.2)' : 'rgba(204, 0, 0, 0.1)', 
                border: bulkEjectState ? '1px solid rgba(100, 100, 100, 0.4)' : '1px solid rgba(204, 0, 0, 0.4)', 
                color: bulkEjectState ? '#aaa' : '#ff4d4d', 
                borderRadius: '4px', cursor: bulkEjectState ? 'wait' : 'pointer', transition: 'all 0.2s ease' 
              }}
              onMouseEnter={(e) => { if(!bulkEjectState) e.currentTarget.style.backgroundColor = 'rgba(204, 0, 0, 0.3)'; }}
              onMouseLeave={(e) => { if(!bulkEjectState) e.currentTarget.style.backgroundColor = 'rgba(204, 0, 0, 0.1)'; }}
            >
              <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"><path d="M3 15v4c0 1.1.9 2 2 2h14a2 2 0 0 0 2-2v-4M12 15V3l-4 4m4-4l4 4"/></svg>
              {bulkEjectState?.type === 'eject' ? (bulkEjectState.calculating ? 'Working...' : `${bulkEjectState.current}/${bulkEjectState.total}`) : 'Eject'}
            </button>
            <button
              className="success icon-button"
              onClick={handleBulkUndoEject}
              disabled={!!bulkEjectState || !hasEjectedFiles}
              title="Restore all globally ejected files"
              style={{
                width: '105px',
                justifyContent: 'center',
                padding: '6px 12px', display: 'flex', alignItems: 'center', gap: '6px', fontSize: '11px',
                backgroundColor: (bulkEjectState || !hasEjectedFiles) ? 'rgba(100, 100, 100, 0.2)' : 'rgba(16, 185, 129, 0.1)',
                border: (bulkEjectState || !hasEjectedFiles) ? '1px solid rgba(100, 100, 100, 0.4)' : '1px solid rgba(16, 185, 129, 0.4)',
                color: (bulkEjectState || !hasEjectedFiles) ? '#aaa' : '#10b981',
                borderRadius: '4px', cursor: (bulkEjectState || !hasEjectedFiles) ? 'not-allowed' : 'pointer', transition: 'all 0.2s ease'
              }}
              onMouseEnter={(e) => { if(!bulkEjectState && hasEjectedFiles) e.currentTarget.style.backgroundColor = 'rgba(16, 185, 129, 0.3)'; }}
              onMouseLeave={(e) => { if(!bulkEjectState && hasEjectedFiles) e.currentTarget.style.backgroundColor = 'rgba(16, 185, 129, 0.1)'; }}
            >
              <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <path d="M3 15v4c0 1.1.9 2 2 2h14a2 2 0 0 0 2-2v-4M12 3v12M8 11l4 4 4-4"/>
              </svg>
              {bulkEjectState?.type === 'restore' ? (bulkEjectState.calculating ? 'Working...' : `${bulkEjectState.current}/${bulkEjectState.total}`) : 'Restore'}
            </button>
          </div>
        </div>
        <div style={{ position: 'relative', flex: 1, minHeight: 0, height: 0, display: 'flex', flexDirection: 'column', borderRadius: 18, border: '1px solid var(--panel-border)', overflow: 'hidden', background: 'var(--bg)' }}>
          <div className="card-stack hide-scrollbars" id="duplicates-stack" ref={cardStackRef} style={{ position: 'relative', overflow: 'auto', flex: 1, border: 'none', borderRadius: 0, width: '100%', paddingRight: '16px' }}>
            <div style={{ height: `${totalHeight}px`, position: 'relative' }}>{renderItems()}</div>
          </div>
          <div style={{ position: 'absolute', right: 4, top: 4, bottom: 4, width: '10px', zIndex: 50, pointerEvents: 'none' }}>
            {totalGroups > 0 && cardStackRef.current && totalHeight > cardStackRef.current.clientHeight && (
              <div 
                onPointerDown={handleThumbPointerDown}
                style={{
                  position: 'absolute',
                  width: '100%',
                  background: 'var(--accent)',
                  borderRadius: '5px',
                  opacity: 0.8,
                  cursor: 'grab',
                  pointerEvents: 'auto',
                  transform: `translateY(${(scrollPercent / 100) * ((cardStackRef.current.clientHeight - 8) - Math.max(20, (cardStackRef.current.clientHeight - 8) * (cardStackRef.current.clientHeight / Math.max(1, totalHeight))))}px)`,
                  height: `${Math.max(20, (cardStackRef.current.clientHeight - 8) * (cardStackRef.current.clientHeight / Math.max(1, totalHeight)))}px`,
              }} />
            )}
          </div>
        </div>
      </section>
    </section>
  );
};

export default DuplicatesPanel;
