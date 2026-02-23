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
  const serverDupeCount = status?.duplicate_groups ?? status?.duplicate_groups_count ?? status?.duplicates ?? 0;
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

  // Update active thumbnail request count, error count, and queue stats periodically for debug panel
  useEffect(() => {
    const updateCounts = () => {
      setActiveThumbnailRequests(getActiveRequestCount());
      setErrorCount(errorLogger.getErrorCount());
      const stats = thumbnailQueue.getStats();
      setQueueStats({ pending: stats.pending, active: stats.active, completed: stats.completed });
    };

    updateCounts(); // Update immediately
    const interval = setInterval(updateCounts, 100); // Update every 100ms for real-time display
    return () => clearInterval(interval);
  }, []);

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
        let hasNewGroups = false;

        // Add new groups to cache, only update version if we actually added new items
        data.groups.forEach((group, idx) => {
          const key = startIndex + idx;
          if (!cache.has(key)) {
            cache.set(key, group);
            hasNewGroups = true;
          }
        });

        if (hasNewGroups) {
          console.log(`Loaded ${data.groups.length} groups, cache size: ${cache.size}`);
          setCacheVersion(v => v + 1); // Trigger re-render only if new groups were added
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

      // Safety check: ensure range is valid
      if (range.start < 0 || range.end < range.start) {
        console.warn('Invalid visible range:', range);
        return;
      }

      // If totalGroups is 0, we need to load initial data first
      if (total === 0) {
        // Only load initial if cache is empty and not already loading
        if (cache.size === 0 && !isLoadingRef.current && loadGroupsFromAPIRef.current) {
          loadGroupsFromAPIRef.current(0, BATCH_SIZE).catch(err => {
            console.error('Error loading initial groups:', err);
          });
        }
        return;
      }

      // Find first missing index in visible range
      let firstMissing = -1;
      let missingCount = 0;

      // Cap the end to totalGroups - 1
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

  // Keep ref in sync
  useEffect(() => {
    checkAndLoadMissingRef.current = checkAndLoadMissing;
  }, [checkAndLoadMissing]);

  // Prune cache to keep only items near the viewport
  const pruneCache = useCallback((currentStart: number, currentEnd: number) => {
    const cache = groupsCacheRef.current;
    if (cache.size <= CACHE_SIZE) return;

    const keepStart = Math.max(0, currentStart - CACHE_SIZE / 2);
    const keepEnd = currentEnd + CACHE_SIZE / 2;

    let deleted = 0;
    for (const key of Array.from(cache.keys())) {
      if (key < keepStart || key > keepEnd) {
        cache.delete(key);
        deleted++;
      }
    }

    if (deleted > 0) {
      console.log(`Pruned ${deleted} items from cache, new size: ${cache.size}`);
    }
  }, []);

  // Refresh visible range without changing scroll - keeps viewport stable when syncing with server
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

    // Cancel any pending RAF
    if (scrollRAFRef.current) {
      cancelAnimationFrame(scrollRAFRef.current);
    }

    scrollRAFRef.current = requestAnimationFrame(() => {
      const container = cardStackRef.current;
      if (!container) return;

      const newRange = calculateVisibleRange(container);

      // Calculate scroll percentage
      const scrollTop = container.scrollTop;
      const scrollHeight = container.scrollHeight - container.clientHeight;
      const percent = scrollHeight > 0 ? Math.round((scrollTop / scrollHeight) * 100) : 0;
      setScrollPercent(percent);

      // Update visible range
      setVisibleRange(prev => {
        if (prev.start === newRange.start && prev.end === newRange.end) {
          return prev;
        }
        return newRange;
      });

      // If viewport changed and there was a recent remap, trigger thumbnail reload
      if (lastRemapTimestampRef.current > 0 && Date.now() - lastRemapTimestampRef.current < 30000) {
        // A remap happened in the last 30 seconds, force thumbnail reload
        setThumbnailReloadKey(prev => prev + 1);
        // Update cache version to force re-render of visible items
        setCacheVersion(v => v + 1);
      }

      // Debounce loading check
      if (loadTimeoutRef.current) {
        clearTimeout(loadTimeoutRef.current);
      }
      loadTimeoutRef.current = setTimeout(() => {
        checkAndLoadMissing();
      }, 100);

      // Schedule cache pruning (debounced)
      if (cleanupTimeoutRef.current) {
        clearTimeout(cleanupTimeoutRef.current);
      }
      cleanupTimeoutRef.current = setTimeout(() => {
        pruneCache(newRange.start, newRange.end);
      }, 2000);

      scrollRAFRef.current = null;
    });
  }, [calculateVisibleRange, checkAndLoadMissing, pruneCache]);

  // Memory monitoring and cleanup - unload images from cards far outside viewport
  useEffect(() => {
    if (!isActive || !isOnline) return;

    const performMemoryCleanup = () => {
      const container = cardStackRef.current;
      if (!container) return;

      const scrollTop = container.scrollTop;
      const containerHeight = container.clientHeight;

      // Calculate a wider range - anything outside this gets cleaned up
      // Use a very large buffer to be conservative and avoid unloading visible images
      const cleanupBuffer = 20; // Keep images loaded for 20 cards outside viewport
      const safeStart = Math.max(0, Math.floor(scrollTop / ITEM_HEIGHT) - cleanupBuffer);
      const safeEnd = Math.min(totalGroupsRef.current - 1, Math.ceil((scrollTop + containerHeight) / ITEM_HEIGHT) + cleanupBuffer);

      // Find all duplicate group cards
      const allCards = container.querySelectorAll('[data-group-index]');
      let unloadedCount = 0;

      allCards.forEach((cardEl) => {
        const index = parseInt(cardEl.getAttribute('data-group-index') || '-1');
        if (index < 0) return;

        // Only unload images from cards that are VERY far outside safe range
        // This prevents unloading images that are still somewhat visible
        if (index < safeStart - 25 || index > safeEnd + 25) {
          const images = cardEl.querySelectorAll('img[src]');
          images.forEach((imgEl) => {
            const img = imgEl as HTMLImageElement;
            if (img.src) {
              // Check if image was recently loaded - don't unload if loaded within last 10 seconds
              // This prevents flickering when scrolling quickly
              const loadTimestamp = img.getAttribute('data-loaded-at');
              if (loadTimestamp) {
                const loadTime = parseInt(loadTimestamp, 10);
                if (Date.now() - loadTime < 10000) {
                  return; // Too recent, don't unload
                }
              }

              // Double-check the image is actually far from viewport using getBoundingClientRect
              const rect = cardEl.getBoundingClientRect();
              const containerRect = container.getBoundingClientRect();
              const distanceFromViewport = Math.min(
                containerRect.top - rect.bottom, // Above viewport
                rect.top - containerRect.bottom  // Below viewport
              );

              // Only unload if more than 1000px away from viewport
              if (distanceFromViewport < 1000) {
                return; // Still close enough, keep loaded
              }

              // Unload image by removing src
              const src = img.src;
              img.removeAttribute('src');
              img.setAttribute('data-src', src);
              img.style.display = 'none';

              // Show placeholder
              const placeholder = img.parentElement?.querySelector('.thumbnail-placeholder') as HTMLElement;
              if (placeholder) {
                placeholder.style.display = 'flex';
              }

              unloadedCount++;
            }
          });
        }
      });

      if (unloadedCount > 0) {
        console.log(`[MemoryCleanup] Unloaded ${unloadedCount} thumbnails from cards outside viewport`);
        lastMemoryCleanupRef.current = Date.now();
      }
    };

    // Run cleanup less frequently - every 10 seconds, and only if scrolling has stopped
    memoryCleanupIntervalRef.current = setInterval(() => {
      // Only cleanup if we haven't cleaned up in the last 5 seconds (to avoid during active scrolling)
      if (Date.now() - lastMemoryCleanupRef.current > 5000) {
        performMemoryCleanup();
      }
    }, 10000); // Less frequent - every 10 seconds

    // Also cleanup after scrolling stops (with longer delay)
    const handleScrollForCleanup = () => {
      if (scrollStopTimeoutRef.current) clearTimeout(scrollStopTimeoutRef.current);
      scrollStopTimeoutRef.current = setTimeout(() => {
        performMemoryCleanup();
      }, 5000); // Wait 5 seconds after scrolling stops before cleanup
    };

    const container = cardStackRef.current;
    if (container) {
      container.addEventListener('scroll', handleScrollForCleanup, { passive: true });
    }

    return () => {
      if (memoryCleanupIntervalRef.current) {
        clearInterval(memoryCleanupIntervalRef.current);
        memoryCleanupIntervalRef.current = null;
      }
      if (scrollStopTimeoutRef.current) {
        clearTimeout(scrollStopTimeoutRef.current);
        scrollStopTimeoutRef.current = null;
      }
      if (container) {
        container.removeEventListener('scroll', handleScrollForCleanup);
      }
    };
  }, [isActive, isOnline]);

  // Listen for remap events to track when remaps occur
  useEffect(() => {
    const handleRemap = () => {
      lastRemapTimestampRef.current = Date.now();
      // Force a cache version update to trigger re-render
      setCacheVersion(v => v + 1);
      console.log('[DuplicatesPanel] Remap detected, will trigger thumbnail reload on viewport change');
    };

    const unsubscribe = remapEventEmitter.subscribe(handleRemap);
    return unsubscribe;
  }, []);

  // Generate comprehensive test report
  const generateTestReport = (
    stats: ScrollTestStats | null,
    duration: number,
    initialErrorCount: number,
    totalGroupsCount: number
  ) => {
    if (!stats) return;

    const finalErrorCount = errorLogger.getErrorCount();
    const errorsDuringTest = finalErrorCount - initialErrorCount;

    // Calculate averages
    const avgQueuePending = stats.queueStats.length > 0
      ? Math.round(stats.queueStats.reduce((sum, s) => sum + s.pending, 0) / stats.queueStats.length)
      : 0;
    const avgQueueActive = stats.queueStats.length > 0
      ? Math.round(stats.queueStats.reduce((sum, s) => sum + s.active, 0) / stats.queueStats.length)
      : 0;
    const avgQueueCompleted = stats.queueStats.length > 0
      ? Math.round(stats.queueStats.reduce((sum, s) => sum + s.completed, 0) / stats.queueStats.length)
      : 0;

    const maxMemoryUsed = stats.memoryStats.length > 0
      ? Math.max(...stats.memoryStats.map(m => m.used))
      : 0;
    const maxMemoryTotal = stats.memoryStats.length > 0
      ? Math.max(...stats.memoryStats.map(m => m.total))
      : 0;
    const avgMemoryUsed = stats.memoryStats.length > 0
      ? Math.round(stats.memoryStats.reduce((sum, m) => sum + m.used, 0) / stats.memoryStats.length)
      : 0;

    const durationMinutes = Math.floor(duration / 60000);
    const durationSeconds = Math.floor((duration % 60000) / 1000);
    const groupsPerSecond = duration > 0 ? (stats.steps / (duration / 1000)).toFixed(2) : '0';

    const report = {
      summary: {
        status: stats.crashes > 0 ? 'FAILED' : 'PASSED',
        duration: `${durationMinutes}m ${durationSeconds}s`,
        totalSteps: stats.steps,
        totalGroups: totalGroupsCount,
        groupsPerSecond,
        crashes: stats.crashes,
        errorsDuringTest,
        initialErrorCount,
        finalErrorCount,
      },
      performance: {
        avgQueuePending,
        avgQueueActive,
        avgQueueCompleted,
        maxMemoryUsedMB: (maxMemoryUsed / 1048576).toFixed(2),
        maxMemoryTotalMB: (maxMemoryTotal / 1048576).toFixed(2),
        avgMemoryUsedMB: (avgMemoryUsed / 1048576).toFixed(2),
      },
      errors: stats.errors,
      queueStats: stats.queueStats.slice(-10), // Last 10 queue stats
      memoryStats: stats.memoryStats.slice(-10), // Last 10 memory stats
    };

    // Display report in console
    console.log('\n' + '='.repeat(80));
    console.log('SCROLL TEST REPORT');
    console.log('='.repeat(80));
    console.log('\n📊 SUMMARY:');
    console.log(`  Status: ${report.summary.status}`);
    console.log(`  Duration: ${report.summary.duration}`);
    console.log(`  Total Steps: ${report.summary.totalSteps}`);
    console.log(`  Total Groups: ${report.summary.totalGroups}`);
    console.log(`  Groups/Second: ${report.summary.groupsPerSecond}`);
    console.log(`  Crashes: ${report.summary.crashes}`);
    console.log(`  Errors During Test: ${report.summary.errorsDuringTest}`);
    console.log(`  Initial Error Count: ${report.summary.initialErrorCount}`);
    console.log(`  Final Error Count: ${report.summary.finalErrorCount}`);

    console.log('\n⚡ PERFORMANCE:');
    console.log(`  Avg Queue - Pending: ${report.performance.avgQueuePending}, Active: ${report.performance.avgQueueActive}, Completed: ${report.performance.avgQueueCompleted}`);
    console.log(`  Memory - Max Used: ${report.performance.maxMemoryUsedMB}MB, Max Total: ${report.performance.maxMemoryTotalMB}MB, Avg Used: ${report.performance.avgMemoryUsedMB}MB`);

    if (report.errors.length > 0) {
      console.log('\n❌ ERRORS DURING TEST:');
      report.errors.forEach((error, i) => {
        const time = new Date(error.timestamp).toLocaleTimeString();
        console.log(`  [${i + 1}] ${time} - ${error.type}: ${error.message}`);
      });
    } else {
      console.log('\n✅ No errors detected during test');
    }

    console.log('\n' + '='.repeat(80));

    // Also display as alert for visibility
    const alertMessage = `Scroll Test Complete!\n\n` +
      `Status: ${report.summary.status}\n` +
      `Duration: ${report.summary.duration}\n` +
      `Steps: ${report.summary.totalSteps}\n` +
      `Crashes: ${report.summary.crashes}\n` +
      `Errors: ${report.summary.errorsDuringTest}\n\n` +
      `See console for full report.`;

    alert(alertMessage);

    // Store report in window for programmatic access
    (window as unknown as { lastScrollTestReport: typeof report }).lastScrollTestReport = report;

    return report;
  };

  // Scroll test function - exposed on window for automation
  const startScrollTest = useCallback(async () => {
    if (scrollTestRunningRef.current) {
      console.log('[ScrollTest] Test already running');
      return;
    }

    if (!isActive) {
      console.error('[ScrollTest] Duplicates panel is not active');
      return;
    }

    const scrollContainer = cardStackRef.current;
    if (!scrollContainer) {
      console.error('[ScrollTest] Could not find duplicates-stack container');
      return;
    }

    const metaElement = document.getElementById('duplicates-meta');
    const totalText = metaElement?.textContent || '';
    const totalMatch = totalText.match(/(\d+)/);
    const totalGroupsCount = totalMatch ? parseInt(totalMatch[1]) : 0;

    console.log(`[ScrollTest] Found ${totalGroupsCount} total groups`);

    if (totalGroupsCount === 0) {
      console.error('[ScrollTest] No groups found');
      return;
    }

    // Check if we're resuming from an interrupted test
    const resumeInfo = (window as unknown as { scrollTestResumeInfo?: { position: number; step: number; stats: ScrollTestStats } }).scrollTestResumeInfo;
    let scrollPosition = 0;
    let step = 0;
    let testStartTime = Date.now();
    let initialErrorCountVal = errorLogger.getErrorCount();

    if (resumeInfo) {
      // Resume from saved position
      scrollPosition = resumeInfo.position || 0;
      step = resumeInfo.step || 0;
      const resumeStats = resumeInfo.stats;
      scrollTestStatsRef.current = resumeStats || {
        startTime: Date.now(),
        steps: step,
        errors: [],
        queueStats: [],
        memoryStats: [],
        crashes: 0,
        totalGroups: totalGroupsCount,
        initialErrorCount: initialErrorCountVal,
        lastScrollPosition: scrollPosition,
      };
      if (scrollTestStatsRef.current) {
        testStartTime = scrollTestStatsRef.current.startTime;
        initialErrorCountVal = scrollTestStatsRef.current.initialErrorCount || initialErrorCountVal;
      }
      scrollContainer.scrollTop = scrollPosition;
      console.log(`[ScrollTest] Resuming from step ${step}, position ${scrollPosition}px`);
      delete (window as unknown as { scrollTestResumeInfo?: unknown }).scrollTestResumeInfo;
    } else {
      // Start new test
      testStartTime = Date.now();
      initialErrorCountVal = errorLogger.getErrorCount();
      scrollTestStatsRef.current = {
        startTime: testStartTime,
        steps: 0,
        errors: [],
        queueStats: [],
        memoryStats: [],
        crashes: 0,
        totalGroups: totalGroupsCount,
        initialErrorCount: initialErrorCountVal,
        lastScrollPosition: 0,
      };
    }

    // Save initial state to localStorage for crash recovery
    try {
      localStorage.setItem(SCROLL_TEST_STORAGE_KEY, JSON.stringify(scrollTestStatsRef.current));
    } catch (e) {
      console.warn('[ScrollTest] Failed to save test state:', e);
    }

    // Set up error monitoring
    const errorHandler = (event: ErrorEvent) => {
      if (scrollTestStatsRef.current) {
        scrollTestStatsRef.current.errors.push({
          timestamp: Date.now(),
          message: event.message || 'Unknown error',
          type: 'error',
        });
        scrollTestStatsRef.current.crashes++;
      }
    };

    const rejectionHandler = (event: PromiseRejectionEvent) => {
      if (scrollTestStatsRef.current) {
        const message = event.reason instanceof Error
          ? event.reason.message
          : String(event.reason || 'Unhandled promise rejection');
        scrollTestStatsRef.current.errors.push({
          timestamp: Date.now(),
          message,
          type: 'unhandledRejection',
        });
        scrollTestStatsRef.current.crashes++;
      }
    };

    window.addEventListener('error', errorHandler);
    window.addEventListener('unhandledrejection', rejectionHandler);

    scrollTestRunningRef.current = true;
    console.log('[ScrollTest] Starting long-running scroll test with crash monitoring...');

    const viewportHeight = scrollContainer.clientHeight;
    const maxScroll = scrollContainer.scrollHeight - scrollContainer.clientHeight;

    console.log(`[ScrollTest] Starting scroll test. Viewport: ${viewportHeight}px, Max scroll: ${maxScroll}px`);
    console.log(`[ScrollTest] Estimated steps: ${Math.ceil(maxScroll / viewportHeight)}`);

    try {
      while (scrollPosition < maxScroll - 10 && scrollTestRunningRef.current) {
        step++;
        const currentPercent = Math.round((scrollPosition / maxScroll) * 100);
        console.log(`[ScrollTest] Step ${step}: Scrolling to ${scrollPosition}px (${currentPercent}%)`);

        scrollContainer.scrollTop = scrollPosition;

        // Use elapsed time tracking - wait 3 seconds between steps
        const waitStart = Date.now();
        const WAIT_DURATION = 3000; // 3 seconds

        while (Date.now() - waitStart < WAIT_DURATION && scrollTestRunningRef.current) {
          // Check every 100ms to be responsive to stop requests
          await new Promise(resolve => setTimeout(resolve, 100));
        }

        if (!scrollTestRunningRef.current) {
          console.log('[ScrollTest] Test stopped by user');
          break;
        }

        const actualScroll = scrollContainer.scrollTop;
        if (actualScroll >= maxScroll - 10) {
          console.log('[ScrollTest] Reached end of scroll');
          break;
        }

        // Collect statistics at each step
        if (scrollTestStatsRef.current) {
          scrollTestStatsRef.current.steps = step;
          scrollTestStatsRef.current.lastScrollPosition = scrollPosition;

          if (window.thumbnailQueue) {
            const stats = window.thumbnailQueue.getStats();
            scrollTestStatsRef.current.queueStats.push({
              step,
              pending: stats.pending,
              active: stats.active,
              completed: stats.completed,
            });
            console.log(`[ScrollTest] Queue stats - P:${stats.pending} A:${stats.active} C:${stats.completed}`);
          }

          // Memory stats
          const perf = performance as { memory?: { usedJSHeapSize: number; totalJSHeapSize: number } };
          const memory = perf.memory;
          if (memory) {
            scrollTestStatsRef.current.memoryStats.push({
              step,
              used: memory.usedJSHeapSize,
              total: memory.totalJSHeapSize,
            });
          }

          // Persist state periodically (every 10 steps) for crash recovery
          if (step % 10 === 0) {
            try {
              localStorage.setItem(SCROLL_TEST_STORAGE_KEY, JSON.stringify(scrollTestStatsRef.current));
            } catch (e) {
              console.warn('[ScrollTest] Failed to save test state:', e);
            }
          }
        }

        scrollPosition += viewportHeight;

        if (step > 2000) {
          console.log('[ScrollTest] Safety limit reached (2000 steps)');
          break;
        }
      }
    } catch (error) {
      console.error('[ScrollTest] Test crashed:', error);
      if (scrollTestStatsRef.current) {
        scrollTestStatsRef.current.errors.push({
          timestamp: Date.now(),
          message: error instanceof Error ? error.message : String(error),
          type: 'crash',
        });
        scrollTestStatsRef.current.crashes++;
      }
    } finally {
      // Clean up event listeners
      window.removeEventListener('error', errorHandler);
      window.removeEventListener('unhandledrejection', rejectionHandler);

      const testEndTime = Date.now();
      const testDuration = testEndTime - testStartTime;

      if (scrollTestStatsRef.current) {
        scrollTestStatsRef.current.endTime = testEndTime;
        scrollTestStatsRef.current.steps = step;
      }

      // Generate and display report
      generateTestReport(scrollTestStatsRef.current, testDuration, initialErrorCountVal, totalGroupsCount);

      // Clear saved state after successful completion
      try {
        localStorage.removeItem(SCROLL_TEST_STORAGE_KEY);
      } catch (e) {
        console.warn('[ScrollTest] Failed to clear test state:', e);
      }

      scrollTestRunningRef.current = false;
    }
  }, [isActive]);

  const stopScrollTest = useCallback(() => {
    if (scrollTestRunningRef.current) {
      scrollTestRunningRef.current = false;
      console.log('[ScrollTest] Test stopped by user');
    }
  }, []);

  // Expose scroll test functions on window
  useEffect(() => {
    window.startScrollTest = startScrollTest;
    window.stopScrollTest = stopScrollTest;
    return () => {
      delete window.startScrollTest;
      delete window.stopScrollTest;
    };
  }, [startScrollTest, stopScrollTest]);

  // Check for interrupted test on mount
  useEffect(() => {
    if (isActive) {
      try {
        const savedTestState = localStorage.getItem(SCROLL_TEST_STORAGE_KEY);
        if (savedTestState) {
          const testState = JSON.parse(savedTestState) as ScrollTestStats;
          // Check if test was running (no endTime) and was recent (within last hour)
          if (!testState.endTime && testState.startTime) {
            const timeSinceStart = Date.now() - testState.startTime;
            const oneHour = 60 * 60 * 1000;

            if (timeSinceStart < oneHour) {
              // Test was interrupted - offer to generate report or resume
              const shouldResume = confirm(
                `A scroll test was interrupted ${Math.round(timeSinceStart / 60000)} minutes ago.\n\n` +
                `Steps completed: ${testState.steps}\n` +
                `Crashes detected: ${testState.crashes}\n\n` +
                `Would you like to:\n` +
                `- Click OK to generate a report from the interrupted test\n` +
                `- Click Cancel to discard and start fresh`
              );

              if (shouldResume) {
                // Ask if user wants to resume or just generate report
                const resumeChoice = confirm(
                  `Would you like to:\n\n` +
                  `OK = Resume test from step ${testState.steps} (scroll position ${testState.lastScrollPosition || 0}px)\n` +
                  `Cancel = Generate report from interrupted test`
                );

                if (resumeChoice) {
                  // Resume the test
                  scrollTestStatsRef.current = testState;

                  // Restore scroll position and continue
                  // Store resume info for the startScrollTest function to pick up
                  (window as unknown as { scrollTestResumeInfo: { position: number; step: number; stats: ScrollTestStats } }).scrollTestResumeInfo = {
                    position: testState.lastScrollPosition || 0,
                    step: testState.steps,
                    stats: testState,
                  };
                  console.log(`[ScrollTest] Resume info stored. Click "Start Scroll Test" to resume from step ${testState.steps}`);
                  alert(`Test will resume from step ${testState.steps} when you click "Start Scroll Test"`);
                } else {
                  // Generate report from interrupted test
                  scrollTestStatsRef.current = testState;
                  if (testState) {
                    generateTestReport(testState, timeSinceStart, testState.initialErrorCount || 0, testState.totalGroups || 0);
                  }
                }
              }

              // Clear saved state
              localStorage.removeItem(SCROLL_TEST_STORAGE_KEY);
            } else {
              // Too old, just clear it
              localStorage.removeItem(SCROLL_TEST_STORAGE_KEY);
            }
          } else if (testState.endTime) {
            // Test completed, just clear old state
            localStorage.removeItem(SCROLL_TEST_STORAGE_KEY);
          }
        }
      } catch (e) {
        console.warn('[ScrollTest] Failed to check for interrupted test:', e);
        localStorage.removeItem(SCROLL_TEST_STORAGE_KEY);
      }
    }
  }, [isActive]);

  // Initialize when panel becomes active
  useEffect(() => {
    if (!isActive || !isOnline) {
      // Reset when inactive
      groupsCacheRef.current.clear();
      setTotalGroups(0);
      totalGroupsRef.current = 0;
      lastTotalGroupsRef.current = 0;
      setVisibleRange({ start: 0, end: 20 });
      setCacheVersion(0);
      isInitializedRef.current = false;
      return;
    }

    // Only initialize once when becoming active, and only if cache is truly empty
    // Use ref to avoid dependency on loadGroupsFromAPI callback
    if (!isInitializedRef.current && groupsCacheRef.current.size === 0 && !isLoadingRef.current && loadGroupsFromAPIRef.current) {
      isInitializedRef.current = true;
      loadGroupsFromAPIRef.current(0, BATCH_SIZE).catch(err => {
        console.error('Error during initialization:', err);
        isInitializedRef.current = false; // Allow retry on error
      });
    }
  }, [isActive, isOnline]);

  // Set up scroll listener
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

  // Re-check for missing groups when loading finishes
  // Use ref to avoid dependency on checkAndLoadMissing callback
  useEffect(() => {
    if (!isLoading && isActive && totalGroups > 0) {
      // Only trigger if totalGroups actually changed (not just re-render)
      if (totalGroups !== lastTotalGroupsRef.current) {
        lastTotalGroupsRef.current = totalGroups;
        const timeoutId = setTimeout(() => {
          checkAndLoadMissingRef.current();
        }, 200);
        return () => clearTimeout(timeoutId);
      }
    }
  }, [isLoading, isActive, totalGroups]);

  // Sync with server when heartbeat shows different duplicate count (groups added or removed)
  useEffect(() => {
    if (!isActive || !isOnline) return;
    if (totalGroups === 0) return; // Initial load handles first fetch
    if (serverDupeCount === totalGroups) return; // Already in sync

    const now = Date.now();
    if (now - lastStatusDrivenRefreshRef.current < STATUS_REFRESH_COOLDOWN_MS) return;

    lastStatusDrivenRefreshRef.current = now;
    refreshVisibleRange();
  }, [isActive, isOnline, serverDupeCount, totalGroups, refreshVisibleRange]);

  // Calculate total scroll height (min 200px when empty so empty-state is visible)
  const totalHeight = totalGroups > 0 ? totalGroups * ITEM_HEIGHT - GAP : 200;

  // Render items in the visible range with absolute positioning
  // Memoize to prevent unnecessary re-renders
  const renderItems = useCallback(() => {
    if (totalGroups === 0) {
      if (isOnline && !isLoading) {
        return (
          <div className="card placeholder duplicates-empty-state" style={{ position: 'relative', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 16, padding: 32 }}>
            <p style={{ margin: 0, color: 'var(--muted)', textAlign: 'center' }}>
              No duplicates found yet. Register a media location to start scanning.
            </p>
            <button
              className="primary"
              onClick={() => setActivePanel('settings')}
              style={{ minWidth: 180 }}
            >
              Add Media Location
            </button>
          </div>
        );
      }
      return (
        <div className="card placeholder" style={{ position: 'relative' }}>
          {isLoading ? 'Loading...' : 'Duplicate cards will appear here…'}
        </div>
      );
    }

    const cache = groupsCacheRef.current;
    const items: React.ReactNode[] = [];

    for (let idx = visibleRange.start; idx <= visibleRange.end && idx < totalGroups; idx++) {
      const top = idx * ITEM_HEIGHT;
      const group = cache.get(idx);

      if (group) {
        items.push(
          <div
            key={`group-${idx}`}
            data-group-index={idx}
            style={{
              position: 'absolute',
              top: `${top}px`,
              left: 0,
              right: 0,
              height: `${CARD_HEIGHT}px`,
              overflow: 'hidden',
            }}
          >
            <DuplicateGroupControl group={group} />
          </div>
        );
      } else {
        items.push(
          <div
            key={`placeholder-${idx}`}
            className="card placeholder"
            style={{
              position: 'absolute',
              top: `${top}px`,
              left: 0,
              right: 0,
              height: `${CARD_HEIGHT}px`,
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              overflow: 'hidden',
            }}
          >
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
            <p className="panel-description">
              Review similar media files grouped by similarity and take action directly from the list.
            </p>
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
          <p className="panel-description">
            Review similar media files grouped by similarity and take action directly from the list.
          </p>
        </div>
      </header>
      <section className="panel duplicates-panel">
        <div className="panel-title split">
          <span>Live Duplicate Feed</span>
          <span className="panel-meta" id="duplicates-meta">
            {totalGroups > 0 ? `${totalGroups} groups` : 'No duplicates'}
          </span>
        </div>
        <div
          className="card-stack"
          id="duplicates-stack"
          ref={cardStackRef}
          style={{
            position: 'relative',
            overflow: 'auto',
          }}
        >
          {/* Fixed height container to maintain scroll position */}
          <div style={{ height: `${totalHeight}px`, position: 'relative' }}>
            {renderItems()}
          </div>
        </div>
      </section>
      {/* Debug Overlay - only when enabled from Settings > Advanced */}
      {debugOverlayEnabled && (
      <div style={{
        position: 'fixed',
        bottom: 10,
        left: 10,
        background: 'rgba(0,0,0,0.8)',
        color: '#0f0',
        padding: 10,
        borderRadius: 5,
        fontSize: 12,
        zIndex: 9999,
        pointerEvents: 'none'
      }}>
        <div>Total: {totalGroups}</div>
        <div>Cache: {groupsCacheRef.current.size}</div>
        <div>Loading: {isLoading ? 'YES' : 'NO'}</div>
        <div>Online: {isOnline ? 'YES' : 'NO'}</div>
        <div>Visible: {visibleRange.start} - {visibleRange.end}</div>
        <div>Scroll: {scrollPercent}%</div>
        <div>Thumbnails: {activeThumbnailRequests}</div>
        <div>Queue: P:{queueStats.pending} A:{queueStats.active} C:{queueStats.completed}</div>
        <div>Errors: {errorCount}</div>
        <div>Objects: {(() => {
          let totalObjects = 0;
          groupsCacheRef.current.forEach((group) => {
            if (group.members && group.members.length > 0) {
              totalObjects += group.members.length;
            } else {
              // Count candidates + representative (which is separate when using candidates)
              totalObjects += (group.candidates?.length || 0) + 1;
            }
          });
          return totalObjects;
        })()}</div>
        <div>Memory: {(() => {
          const perf = performance as { memory?: { usedJSHeapSize: number; totalJSHeapSize: number } };
          const memory = perf.memory;
          if (memory) {
            const usedMB = (memory.usedJSHeapSize / 1048576).toFixed(2);
            const totalMB = (memory.totalJSHeapSize / 1048576).toFixed(2);
            return `${usedMB}MB / ${totalMB}MB`;
          }
          return 'N/A';
        })()}</div>
        <div>Ver: {cacheVersion}</div>
      </div>
      )}
    </section>
  );
};

export default DuplicatesPanel;
