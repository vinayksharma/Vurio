import React, { useRef, useEffect, useState, useMemo } from 'react';
import type { DuplicateGroup, DuplicateGroupMember } from '../types';
import { API_BASE, ejectDuplicates } from '../utils/api';
import { remapEventEmitter, type RemapEvent } from '../utils/remapEvents';
import { getActiveRequestCount } from '../utils/thumbnailRequests';
import { errorLogger } from '../utils/errorLogger';
import { thumbnailQueue } from '../utils/thumbnailQueue';
import { useUIStore } from '../stores';

interface DuplicateGroupControlProps {
  group: DuplicateGroup;
}

const DuplicateGroupControl: React.FC<DuplicateGroupControlProps> = ({ group }) => {
  const duplicateTightness = useUIStore((state) => state.duplicateTightness);
  const [showAll, setShowAll] = useState(false);
  const containerRef = useRef<HTMLDivElement>(null);
  const horizontalScrollRef = useRef<HTMLDivElement>(null);
  const scrollContentRef = useRef<HTMLDivElement>(null); // Inner wrapper so bracket scrolls with thumbnails
  const groupObserverRef = useRef<IntersectionObserver | null>(null);
  const thumbnailObserversRef = useRef<Map<HTMLElement, IntersectionObserver>>(new Map()); // Track individual thumbnail observers
  const [remapKey, setRemapKey] = useState<number>(0); // Force re-render after remap
  const isGroupVisibleRef = useRef<boolean>(false);
  const loadedImagesRef = useRef<Set<string>>(new Set()); // Track loaded images
  const imageSrcRef = useRef<Map<string, string>>(new Map()); // Track original src URLs for unloading
  const queuedUrlsRef = useRef<Set<string>>(new Set()); // Track URLs queued for this group (for cancellation)
  const groupRef = useRef<DuplicateGroup>(group);
  const [bracketPosition, setBracketPosition] = useState<{ left: number; width: number } | null>(null);
  const [isEjecting, setIsEjecting] = useState(false);

  // Keep ref updated with latest group
  useEffect(() => {
    groupRef.current = group;
  }, [group]);

  // Use members array if available, sorted by similarity (highest first)
  const sortedMembers = useMemo((): DuplicateGroupMember[] => {
    if (group.members && group.members.length > 0) {
      const sorted = [...group.members].sort((a, b) => (b.similarity_score || 0) - (a.similarity_score || 0));

      // Ensure representative is included
      const representativeInMembers = sorted.some(m => m.file_path === group.representative);
      if (!representativeInMembers) {
        sorted.unshift({
          file_path: group.representative,
          similarity_score: 1.0,
          is_representative: true,
          file_size: 0,
        });
      }
      return sorted;
    } else {
      // Fallback to old structure
      return [
        {
          file_path: group.representative,
          similarity_score: 1.0,
          is_representative: true,
          file_size: 0
        },
        ...(group.candidates || []).map(path => ({
          file_path: path,
          similarity_score: group.similarity_threshold || 0.9,
          file_size: 0
        })),
      ];
    }
  }, [group.members, group.representative, group.candidates, group.similarity_threshold]);

  if (!group) return null;

  const totalFileCount = group.file_count || sortedMembers.length;
  const header = `Group #${group.id} • ${totalFileCount} files`;

  // Limit displayed members if not showing all
  const INITIAL_LIMIT = 20;
  const hasMore = sortedMembers.length > INITIAL_LIMIT;
  const displayedMembers = useMemo(
    () => showAll ? sortedMembers : sortedMembers.slice(0, INITIAL_LIMIT),
    [showAll, sortedMembers]
  );
  const remainingCount = sortedMembers.length - INITIAL_LIMIT;

  const handleWheel = (e: React.WheelEvent<HTMLDivElement>) => {
    if (e.deltaY !== 0) {
      e.preventDefault();
      const target = e.currentTarget;
      target.scrollLeft += e.deltaY;
    }
  };

  const handleImageClick = (filePath: string, tooltip: string) => {
    if (window.imagePopup) {
      window.imagePopup.open({ filePath, details: tooltip });
    }
  };

  // Helper function to check if a file path is affected by a remap
  const isPathAffected = (filePath: string, oldPath: string): boolean => {
    return filePath.startsWith(oldPath + '/') || filePath === oldPath;
  };

  // Listen for remap events and invalidate affected thumbnails
  useEffect(() => {
    const handleRemap = (event: RemapEvent) => {
      const currentGroup = groupRef.current;

      const allPaths = [
        currentGroup.representative,
        ...(currentGroup.members?.map(m => m.file_path) || []),
        ...(currentGroup.candidates || []),
      ];

      const hasAffectedPath = allPaths.some(path => isPathAffected(path, event.oldPath));

      if (hasAffectedPath) {
        console.log(`[DuplicateGroupControl] Group ${currentGroup.id} has paths affected by remap, invalidating thumbnails`);

        // Collect URLs to clear from queue
        const urlsToClear: string[] = [];
        allPaths.forEach(path => {
          if (isPathAffected(path, event.oldPath)) {
            const url = `${API_BASE}/api/v1/thumbnails?path=${encodeURIComponent(path)}&size=128`;
            urlsToClear.push(url);
          }
        });

        // Clear completed status in queue for remapped paths
        if (urlsToClear.length > 0) {
          thumbnailQueue.clearCompleted(urlsToClear);
        }

        // Clear loaded images cache to force re-loading
        loadedImagesRef.current.clear();

        // Increment remap key to force re-render
        setRemapKey(prev => prev + 1);
      }
    };

    const unsubscribe = remapEventEmitter.subscribe(handleRemap);
    return unsubscribe;
  }, [group.id]);

  // Group-level IntersectionObserver: when group is visible, load all thumbnails
  useEffect(() => {
    const scrollContainer = document.getElementById('duplicates-stack');
    if (!scrollContainer || !containerRef.current) return;

    // Clean up old observer if it exists
    if (groupObserverRef.current) {
      groupObserverRef.current.disconnect();
      groupObserverRef.current = null;
    }

    // Load all thumbnails function - now uses queue
    const loadAllThumbnails = () => {
      if (!containerRef.current) return;

      try {
        // Get all displayed members from DOM (in case displayedMembers changed)
        const memberElements = containerRef.current.querySelectorAll('.duplicate-thumbnail');
        const activeRequestCount = getActiveRequestCount();

        // Log if we're about to create many requests
        if (memberElements.length > 20) {
          console.warn(`[DuplicateGroupControl] Queueing ${memberElements.length} thumbnails for group ${group.id}, active requests: ${activeRequestCount}`);
        }

        const urlsToQueue: string[] = [];

        memberElements.forEach((thumbnailDiv, index) => {
          try {
            const img = thumbnailDiv.querySelector('img[data-src]') as HTMLImageElement;
            if (!img || !img.dataset.src) return;

            const src = img.dataset.src;

            // Skip if already loaded
            if (loadedImagesRef.current.has(src)) return;

            // Track URL for cancellation later
            urlsToQueue.push(src);
            queuedUrlsRef.current.add(src);

            // Push to queue (LIFO - newest requests get priority)
            thumbnailQueue.push(
              src,
              img,
              thumbnailDiv as HTMLElement,
              {
                onLoad: (url: string) => {
                  // Successfully loaded
                  loadedImagesRef.current.add(url);
                  imageSrcRef.current.set(url, url);
                  queuedUrlsRef.current.delete(url);
                },
                onError: (url: string, error: Error) => {
                  // Failed to load
                  errorLogger.logThumbnailError(url, error, {
                    groupId: group.id,
                    memberIndex: index,
                    activeRequests: getActiveRequestCount(),
                    elementExists: !!img,
                    dataSrc: img.dataset.src,
                  });
                  queuedUrlsRef.current.delete(url);
                },
              },
              {
                groupId: group.id,
                memberIndex: index,
              }
            );
          } catch (err) {
            const error = err instanceof Error ? err : new Error(String(err));
            errorLogger.logThumbnailError('unknown', error, {
              groupId: group.id,
              memberIndex: index,
              phase: 'setup',
              activeRequests: getActiveRequestCount(),
            });
          }
        });

        if (urlsToQueue.length > 0) {
          console.log(`[DuplicateGroupControl] Queued ${urlsToQueue.length} thumbnails for group ${group.id}`);
        }
      } catch (err) {
        const error = err instanceof Error ? err : new Error(String(err));
        errorLogger.logError({
          type: 'thumbnail',
          message: `Error in loadAllThumbnails for group ${group.id}: ${error.message}`,
          stack: error.stack,
          context: {
            groupId: group.id,
            activeRequests: getActiveRequestCount(),
          },
        });
      }
    };

    // Unload all thumbnails function - also cancel pending queue requests
    const unloadAllThumbnails = () => {
      if (!containerRef.current) return;

      // Collect URLs that were loaded/unloaded
      const urlsToClear: string[] = [];

      // Don't cancel pending requests - let them become fire-and-forget
      // This allows server-side caching even when scrolled away
      // When user scrolls back, thumbnails will be cached and load quickly
      queuedUrlsRef.current.clear();

      // Unload all images that are currently loaded
      containerRef.current.querySelectorAll('img[src]').forEach((imgElement) => {
        const img = imgElement as HTMLImageElement;
        const src = img.src;

        if (src) {
          // Get the original data-src or construct it
          const dataSrc = imageSrcRef.current.get(src) || src;

          // Remove src to free memory
          img.removeAttribute('src');
          img.setAttribute('data-src', dataSrc);
          img.style.display = 'none';

          // Show placeholder and hide error
          const thumbnailDiv = img.parentElement as HTMLElement;
          if (thumbnailDiv) {
            const placeholder = thumbnailDiv.querySelector('.thumbnail-placeholder') as HTMLElement;
            if (placeholder) placeholder.style.display = 'flex';
            
            const errorIndicator = thumbnailDiv.querySelector('.thumbnail-error') as HTMLElement;
            if (errorIndicator) errorIndicator.style.display = 'none';
          }
          loadedImagesRef.current.delete(src);
          urlsToClear.push(src);
        }
      });

      // Clear completed status in queue so these can be re-queued when scrolled back
      if (urlsToClear.length > 0) {
        thumbnailQueue.clearCompleted(urlsToClear);
      }

      // Clear tracking maps
      imageSrcRef.current.clear();
    };

    const observer = new IntersectionObserver(
      (entries) => {
        entries.forEach((entry) => {
          if (entry.isIntersecting) {
            // Group is visible - load all thumbnails and set up individual observers
            if (!isGroupVisibleRef.current) {
              isGroupVisibleRef.current = true;
              // Small delay to ensure DOM is ready
              setTimeout(() => {
                loadAllThumbnails();
                // Trigger thumbnail observer setup by forcing a re-render
                // The thumbnail observer effect will check isGroupVisibleRef.current
                setRemapKey(prev => prev + 1);
              }, 0);
            }
          } else {
            // Group is not visible - unload all thumbnails
            if (isGroupVisibleRef.current) {
              isGroupVisibleRef.current = false;
              unloadAllThumbnails();
            }
          }
        });
      },
      {
        root: scrollContainer,
        rootMargin: '100px', // Start loading before fully visible
        threshold: 0.01,
      }
    );

    groupObserverRef.current = observer;
    observer.observe(containerRef.current);

    // Also trigger load if group is already visible on mount
    const rect = containerRef.current.getBoundingClientRect();
    const containerRect = scrollContainer.getBoundingClientRect();
    if (rect.bottom > containerRect.top && rect.top < containerRect.bottom) {
      isGroupVisibleRef.current = true;
      setTimeout(() => {
        loadAllThumbnails();
        // Trigger thumbnail observer setup
        setRemapKey(prev => prev + 1);
      }, 0);
    }

    return () => {
      if (groupObserverRef.current) {
        groupObserverRef.current.disconnect();
        groupObserverRef.current = null;
      }
    };
  }, [group.id, remapKey, displayedMembers, showAll]);

  // Use IntersectionObserver for each thumbnail to detect when it comes into view
  // This works regardless of how it comes into view (scrolling, expanding list, etc.)
  useEffect(() => {
    const scrollContainer = horizontalScrollRef.current;
    if (!scrollContainer) return;

    // Only set up observers if group is visible
    if (!isGroupVisibleRef.current) {
      // Clean up any existing observers
      thumbnailObserversRef.current.forEach((observer) => {
        observer.disconnect();
      });
      thumbnailObserversRef.current.clear();
      return;
    }

    // Clean up old observers
    thumbnailObserversRef.current.forEach((observer) => {
      observer.disconnect();
    });
    thumbnailObserversRef.current.clear();

    // Set up IntersectionObserver for each thumbnail
    const memberElements = containerRef.current?.querySelectorAll('.duplicate-thumbnail');
    if (!memberElements) return;

    const observerOptions = {
      root: scrollContainer,
      rootMargin: '50px', // Start loading slightly before fully visible
      threshold: 0.01, // Trigger when any part becomes visible
    };

    memberElements.forEach((thumbnailDiv) => {
      const img = thumbnailDiv.querySelector('img[data-src]') as HTMLImageElement;
      if (!img || !img.dataset.src) return;

      const src = img.dataset.src;

      // Skip if already loaded
      if (loadedImagesRef.current.has(src)) return;

      const observer = new IntersectionObserver(
        (entries) => {
          entries.forEach((entry) => {
            if (entry.isIntersecting) {
              // Thumbnail is now visible - queue it for loading
              const thumbnailElement = entry.target as HTMLElement;
              const imgElement = thumbnailElement.querySelector('img[data-src]') as HTMLImageElement;
              if (!imgElement || !imgElement.dataset.src) return;

              const thumbnailSrc = imgElement.dataset.src;

              // Skip if already loaded
              if (loadedImagesRef.current.has(thumbnailSrc)) {
                observer.disconnect();
                thumbnailObserversRef.current.delete(thumbnailElement);
                return;
              }

              // Queue the thumbnail
              if (!queuedUrlsRef.current.has(thumbnailSrc)) {
                queuedUrlsRef.current.add(thumbnailSrc);
                thumbnailQueue.push(
                  thumbnailSrc,
                  imgElement,
                  thumbnailElement,
                  {
                    onLoad: (url: string) => {
                      loadedImagesRef.current.add(url);
                      imageSrcRef.current.set(url, url);
                      queuedUrlsRef.current.delete(url);
                      // Disconnect observer once loaded
                      observer.disconnect();
                      thumbnailObserversRef.current.delete(thumbnailElement);
                    },
                    onError: (url: string, error: Error) => {
                      errorLogger.logThumbnailError(url, error, {
                        groupId: group.id,
                        memberIndex: -1,
                        activeRequests: getActiveRequestCount(),
                        elementExists: !!imgElement,
                        dataSrc: imgElement.dataset.src,
                      });
                      queuedUrlsRef.current.delete(url);
                    },
                  },
                  {
                    groupId: group.id,
                    memberIndex: -1,
                  }
                );
              }
            }
          });
        },
        observerOptions
      );

      observer.observe(thumbnailDiv as HTMLElement);
      thumbnailObserversRef.current.set(thumbnailDiv as HTMLElement, observer);
    });

    return () => {
      // Clean up observers
      thumbnailObserversRef.current.forEach((observer) => {
        observer.disconnect();
      });
      thumbnailObserversRef.current.clear();
    };
  }, [group.id, displayedMembers, showAll, remapKey]);

  // Measure actual positions of perfect match thumbnails for accurate bracket positioning
  // Uses scrollContentRef (inner wrapper) so bracket coordinates are relative to scrollable content
  useEffect(() => {
    if (!scrollContentRef.current || !isGroupVisibleRef.current) {
      setBracketPosition(null);
      return;
    }

    const perfectMatches = displayedMembers
      .map((m, idx) => ({ member: m, index: idx }))
      .filter(({ member, index }) => (1 - (member.similarity_score ?? 0)) <= duplicateTightness && index > 0);

    if (perfectMatches.length === 0) {
      setBracketPosition(null);
      return;
    }

    const measure = () => {
      const content = scrollContentRef.current;
      if (!content) return;

      const thumbnails = content.querySelectorAll('.perfect-match-thumbnail');
      if (thumbnails.length === 0) return;

      const first = thumbnails[0] as HTMLElement;
      const last = thumbnails[thumbnails.length - 1] as HTMLElement;
      if (!first || !last) return;

      // offsetLeft/offsetWidth relative to scroll content - bracket scrolls with content
      const left = first.offsetLeft;
      const width = last.offsetLeft + last.offsetWidth - first.offsetLeft;
      setBracketPosition({ left, width });
    };

    requestAnimationFrame(measure);
  }, [displayedMembers, showAll, remapKey, duplicateTightness]);

  // No scroll handler needed - bracket is inside scroll content and moves with thumbnails

  const handleEjectClick = async () => {
    if (isEjecting || !bracketPosition) return;
    
    // Find all perfect match members (those under the bracket)
    const perfectMatches = displayedMembers
      .filter((member, index) => (1 - (member.similarity_score ?? 0)) <= duplicateTightness && index > 0 && !member.is_ejected);
      
    if (perfectMatches.length === 0) return;

    const filePaths = perfectMatches.map(m => m.file_path);
    setIsEjecting(true);

    try {
      const result = await ejectDuplicates(filePaths);
      if (result.success) {
        // Find these members and mark them as ejected locally immediately
        groupRef.current.members?.forEach(m => {
          if (filePaths.includes(m.file_path)) {
            m.is_ejected = true;
          }
        });
        setRemapKey(prev => prev + 1); // trigger re-render
      } else {
        console.error('Failed to eject duplicates:', result.error);
        if (window.errorLogger) {
          window.errorLogger.logError({
            type: 'react',
            message: 'Eject failed',
            context: { error: result.error }
          });
        }
      }
    } catch (err) {
      console.error('Exception during eject:', err);
    } finally {
      setIsEjecting(false);
    }
  };

  const createThumbnail = (member: DuplicateGroupMember, memberIndex: number) => {
    const isFirst = memberIndex === 0;
    const hasPerfectMatch = (1 - (member.similarity_score ?? 0)) <= duplicateTightness && !isFirst; // Don't show on key image
    const rankText = isFirst ? 'Highest Similarity' : `Rank #${memberIndex + 1}`;
    const tooltip = `Group #${group.id} - ${rankText}\nPath: ${member.file_path}\nSimilarity: ${member.similarity_score != null ? member.similarity_score.toFixed(4) : 'N/A'}\nFile Size: ${member.file_size ? (member.file_size / 1024 / 1024).toFixed(1) + 'MB' : 'Unknown'}\nAdded: ${member.added_at ? new Date(member.added_at).toLocaleString() : 'Unknown'}\nRepresentative: ${member.is_representative ? 'Yes' : 'No'}`;

    const thumbnailUrl = `${API_BASE}/api/v1/thumbnails?path=${encodeURIComponent(member.file_path)}&size=128`;

    const thumbnailStyle: React.CSSProperties = isFirst
      ? { border: '2px solid #FF8C00', flexShrink: 0, marginLeft: '4px' }
      : { width: '64px', height: '64px', flexShrink: 0 };

    const handleImageError = (e: React.SyntheticEvent<HTMLImageElement>) => {
      const target = e.target as HTMLImageElement;
      target.style.display = 'none';
      const thumbnailDiv = target.parentElement;
      if (thumbnailDiv) {
        const placeholder = thumbnailDiv.querySelector('.thumbnail-placeholder') as HTMLElement;
        if (placeholder) placeholder.style.display = 'none';
        
        const errorIndicator = thumbnailDiv.querySelector('.thumbnail-error') as HTMLElement;
        if (errorIndicator) errorIndicator.style.display = 'flex';
      }
    };

    return (
      <React.Fragment key={member.file_path}>
        <div
          className={`duplicate-thumbnail ${isFirst ? 'key-thumbnail' : ''} ${hasPerfectMatch ? 'perfect-match-thumbnail' : ''}`}
          title={tooltip}
          onClick={() => handleImageClick(member.file_path, tooltip)}
          style={{ ...thumbnailStyle, position: 'relative' }}
          data-member-index={memberIndex}
        >
          {member.is_ejected && (
            <div style={{
              position: 'absolute',
              inset: 0,
              backgroundColor: 'rgba(0, 0, 0, 0.6)',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              zIndex: 5,
              borderRadius: isFirst ? '2px' : '4px',
              border: '2px solid rgba(255, 0, 0, 0.4)'
            }}>
              <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ff4d4d" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <path d="M3 15v4c0 1.1.9 2 2 2h14a2 2 0 0 0 2-2v-4M12 15V3l-4 4m4-4l4 4"/>
              </svg>
            </div>
          )}
          <img
            data-src={thumbnailUrl}
            alt=""
            loading="lazy"
            decoding="async"
            style={{ display: 'none' }}
            onError={handleImageError}
          />
          <div className="thumbnail-placeholder" style={{ display: 'flex' }}>
            <div className="thumbnail-loading-spinner"></div>
          </div>
          <div className="thumbnail-error" style={{ display: 'none' }}>
            <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" style={{ color: 'rgba(255, 255, 255, 0.4)' }}>
              <circle cx="12" cy="12" r="10"></circle>
              <line x1="15" y1="9" x2="9" y2="15"></line>
              <line x1="9" y1="9" x2="15" y2="15"></line>
            </svg>
          </div>
        </div>
        {isFirst && (
          <div
            style={{
              width: '2px',
              height: '64px',
              background: 'rgba(255,255,255,0.2)',
              margin: '0 8px',
            }}
          />
        )}
      </React.Fragment>
    );
  };

  return (
    <div className="card duplicate-group-card" ref={containerRef} style={{ height: '100%', maxHeight: '100%', minHeight: 0 }}>
      <div style={{ width: '100%', height: '100%', display: 'flex', flexDirection: 'column', minWidth: 0, maxWidth: '100%', boxSizing: 'border-box' }}>
        <div
          className="duplicate-group-header"
          style={{
            fontSize: '12px',
            color: 'var(--text)',
            marginBottom: '8px',
            fontWeight: 600,
            minWidth: 0,
            overflow: 'hidden',
            textOverflow: 'ellipsis',
            whiteSpace: 'nowrap',
          }}
        >
          {header}
        </div>
        <div
          ref={horizontalScrollRef}
          className="thumbnail-scroll-container"
          style={{
            flex: 1,
            overflowX: 'auto',
            overflowY: 'hidden',
            scrollBehavior: 'smooth',
            minWidth: 0,
            maxWidth: '100%',
            width: '100%',
          }}
          onWheel={handleWheel}
        >
          <div
            ref={scrollContentRef}
            style={{
              display: 'flex',
              gap: '12px',
              alignItems: 'center',
              paddingBottom: '8px',
              minWidth: 'min-content',
              position: 'relative',
            }}
          >
            {displayedMembers.map((member, memberIndex) => createThumbnail(member, memberIndex))}

            {/* Continuous bracket for perfect matches - scrolls with thumbnails */}
            {bracketPosition && (
              <div
                style={{
                  position: 'absolute',
                  bottom: '0px',
                  left: `${bracketPosition.left}px`,
                  width: `${bracketPosition.width}px`,
                  height: '2px',
                  background: '#a85858',
                  zIndex: 10,
                  pointerEvents: 'none',
                }}
              >
                {/* Bracket line */}
                <div style={{ width: '100%', height: '100%', background: '#a85858' }} />

                {/* Left vertical bracket extension */}
                <div
                  style={{
                    position: 'absolute',
                    left: 0,
                    bottom: 0,
                    width: '2px',
                    height: '6px',
                    background: '#a85858',
                  }}
                />
                {/* Right vertical bracket extension */}
                <div
                  style={{
                    position: 'absolute',
                    right: 0,
                    bottom: 0,
                    width: '2px',
                    height: '6px',
                    background: '#a85858',
                  }}
                />

                {/* Eject Button Overlay */}
                <div
                  onClick={handleEjectClick}
                  style={{
                    position: 'absolute',
                    top: '-10px',
                    left: '50%',
                    transform: 'translateX(-50%)',
                    width: '20px',
                    height: '20px',
                    borderRadius: '50%',
                    backgroundColor: '#cc0000',
                    border: '1px solid #1a1a1a',
                    display: 'flex',
                    alignItems: 'center',
                    justifyContent: 'center',
                    cursor: isEjecting ? 'wait' : 'pointer',
                    pointerEvents: 'auto',
                    boxShadow: '0 2px 4px rgba(0,0,0,0.5)',
                    opacity: isEjecting ? 0.6 : 1,
                    transition: 'transform 0.1s, opacity 0.2s'
                  }}
                  title="Eject perfect duplicates"
                  onMouseEnter={(e) => { e.currentTarget.style.transform = 'translateX(-50%) scale(1.1)'; e.currentTarget.style.backgroundColor = '#ff1a1a'; }}
                  onMouseLeave={(e) => { e.currentTarget.style.transform = 'translateX(-50%) scale(1)'; e.currentTarget.style.backgroundColor = '#cc0000'; }}
                  onMouseDown={(e) => { e.currentTarget.style.transform = 'translateX(-50%) scale(0.95)'; }}
                  onMouseUp={(e) => { e.currentTarget.style.transform = 'translateX(-50%) scale(1.1)'; }}
                >
                  {isEjecting ? (
                    <div style={{
                      width: '10px',
                      height: '10px',
                      border: '2px solid rgba(255,255,255,0.3)',
                      borderTop: '2px solid #fff',
                      borderRadius: '50%',
                      animation: 'spin 1s linear infinite'
                    }} />
                  ) : (
                    <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="white" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round">
                      <path d="M12 2v10M12 2l-3 3M12 2l3 3M4 14v4a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-4"/>
                    </svg>
                  )}
                </div>
              </div>
            )}

            {hasMore && !showAll && (
              <div
                className="show-more-card"
                onClick={() => setShowAll(true)}
                style={{
                  width: '64px',
                  height: '64px',
                  flexShrink: 0,
                  display: 'flex',
                  flexDirection: 'column',
                  alignItems: 'center',
                  justifyContent: 'center',
                  background: 'rgba(255, 255, 255, 0.05)',
                  borderRadius: '4px',
                  cursor: 'pointer',
                  border: '1px dashed rgba(255, 255, 255, 0.2)',
                  color: 'var(--text-secondary)',
                  fontSize: '11px',
                  textAlign: 'center',
                  padding: '4px'
                }}
              >
                <span>+{remainingCount}</span>
                <span>more</span>
              </div>
            )}
          </div>
        </div>
      </div>
    </div>
  );
};

export default DuplicateGroupControl;
