// Thumbnail request queue with LIFO (Last In First Out) priority
// Max 5 concurrent requests, deduplication, retry with backoff

import { errorLogger } from './errorLogger';
import type { ThumbnailRequest, RequestCallbacks } from '../types';

class ThumbnailRequestQueue {
  private pendingStack: ThumbnailRequest[] = []; // LIFO stack
  private activeRequests = new Map<string, ThumbnailRequest>(); // URL -> Request
  private completedRequests = new Set<string>(); // Track completed URLs for deduplication
  private maxConcurrent = 5;
  private processing = false;
  private processorInterval: ReturnType<typeof setInterval> | null = null;
  private lock = false; // Simple lock to prevent concurrent modifications
  private visibilityCheckInterval: ReturnType<typeof setInterval> | null = null;

  // Retry configuration
  private readonly MAX_RETRIES = 3;
  private readonly INITIAL_RETRY_DELAY = 1000; // 1 second
  private readonly MAX_RETRY_DELAY = 10000; // 10 seconds

  constructor() {
    this.startProcessor();
    this.startVisibilityChecker();
  }

  /**
   * Push a request to the queue (LIFO - added to end, processed from end)
   */
  public push(
    url: string,
    imgElement: HTMLImageElement | null,
    thumbnailDiv: HTMLElement | null,
    callbacks?: RequestCallbacks,
    context?: { groupId?: number; memberIndex?: number }
  ): void {
    // Don't add if currently active
    if (this.activeRequests.has(url)) {
      return;
    }

    // If already completed, check if element still needs loading
    // (element might have been unloaded or remapped)
    if (this.completedRequests.has(url)) {
      // If element has data-src, it needs loading again - clear completed flag
      if (imgElement && imgElement.hasAttribute('data-src')) {
        this.completedRequests.delete(url);
      } else {
        // Element already loaded and doesn't need reloading
        return;
      }
    }

    // Check if already in pending stack
    const existingIndex = this.pendingStack.findIndex(req => req.id === url);
    if (existingIndex !== -1) {
      // Remove from current position to re-add at end (LIFO)
      this.pendingStack.splice(existingIndex, 1);
    }

    // Create request object
    const request: ThumbnailRequest = {
      id: url,
      url,
      imgElement,
      thumbnailDiv,
      callback: callbacks
        ? (success: boolean, url: string) => {
          if (success && callbacks.onLoad) {
            callbacks.onLoad(url);
          } else if (!success && callbacks.onError) {
            callbacks.onError(url, new Error('Request failed'));
          }
        }
        : undefined,
      retryCount: 0,
      nextRetryTime: 0,
      priority: 0,
      groupId: context?.groupId,
      memberIndex: context?.memberIndex,
      addedAt: Date.now(),
    };

    // Cap pending stack to prevent OOM when scrolling through many groups
    const MAX_PENDING = 400;
    if (this.pendingStack.length >= MAX_PENDING) {
      this.pendingStack = this.pendingStack.slice(-Math.floor(MAX_PENDING * 0.7));
    }

    this.pendingStack.push(request);
  }

  /**
   * Remove requests for URLs that are no longer visible
   */
  public cancelForUrls(urls: string[]): void {
    if (this.lock) {
      // Retry after a short delay if locked
      setTimeout(() => this.cancelForUrls(urls), 10);
      return;
    }

    this.lock = true;
    try {
      const urlSet = new Set(urls);

      // Remove from pending stack
      this.pendingStack = this.pendingStack.filter(req => !urlSet.has(req.url));

      // Note: We don't cancel active requests, let them complete
      // This prevents wasted bandwidth and ensures consistency
    } finally {
      this.lock = false;
    }
  }

  /**
   * Cancel all pending requests (not active ones)
   */
  public cancelAll(): void {
    if (this.lock) {
      setTimeout(() => this.cancelAll(), 10);
      return;
    }

    this.lock = true;
    try {
      this.pendingStack = [];
    } finally {
      this.lock = false;
    }
  }

  /**
   * Clear completed status for specific URLs (e.g., when unloaded or remapped)
   * This allows them to be re-queued if needed
   */
  public clearCompleted(urls: string[]): void {
    if (this.lock) {
      setTimeout(() => this.clearCompleted(urls), 10);
      return;
    }

    this.lock = true;
    try {
      const urlSet = new Set(urls);
      urlSet.forEach(url => this.completedRequests.delete(url));
    } finally {
      this.lock = false;
    }
  }

  /**
   * Check if an image element is currently visible in the viewport
   * Accounts for both vertical scrolling (main container) and horizontal scrolling (group container)
   */
  private isElementVisible(element: HTMLElement | null): boolean {
    if (!element) return false;

    try {
      const rect = element.getBoundingClientRect();
      const scrollContainer = document.getElementById('duplicates-stack');

      if (!scrollContainer) return false;

      const containerRect = scrollContainer.getBoundingClientRect();

      // Check vertical visibility against main scroll container
      const verticalMargin = 200; // Extra margin to keep loading slightly off-screen
      const isVerticallyVisible = (
        rect.bottom > containerRect.top - verticalMargin &&
        rect.top < containerRect.bottom + verticalMargin
      );

      if (!isVerticallyVisible) return false;

      // Check horizontal visibility within the group's scroll container
      // Find the parent horizontal scroll container (thumbnail-scroll-container)
      let parent = element.parentElement;
      let horizontalScrollContainer: HTMLElement | null = null;

      while (parent) {
        if (parent.classList.contains('thumbnail-scroll-container') ||
          (parent.style.overflowX === 'auto' || parent.style.overflowX === 'scroll')) {
          horizontalScrollContainer = parent;
          break;
        }
        parent = parent.parentElement;
      }

      // If no horizontal scroll container found, assume visible (backward compatibility)
      if (!horizontalScrollContainer) {
        return (
          rect.right > containerRect.left - verticalMargin &&
          rect.left < containerRect.right + verticalMargin
        );
      }

      // Check horizontal visibility within the group's scroll container
      const horizontalContainerRect = horizontalScrollContainer.getBoundingClientRect();
      const horizontalMargin = 100; // Smaller margin for horizontal scrolling

      return (
        rect.right > horizontalContainerRect.left - horizontalMargin &&
        rect.left < horizontalContainerRect.right + horizontalMargin
      );
    } catch {
      return false;
    }
  }

  /**
   * Periodically check visibility and mark off-screen requests as "fire and forget"
   * This allows server-side caching without blocking UI or waiting for response
   */
  private startVisibilityChecker(): void {
    if (this.visibilityCheckInterval) return;
    this.visibilityCheckInterval = setInterval(() => {
      if (this.lock) return;

      // Check pending requests - mark off-screen ones as "fire and forget"
      // We only check pending, not active (let active requests complete)
      // Don't cancel - just mark as fire-and-forget so they cache on server
      for (const request of this.pendingStack) {
        // If element doesn't exist, cancel it (element was removed from DOM)
        if (!request.thumbnailDiv) {
          // Element removed from DOM - cancel (can't fire-and-forget if no element)
          this.cancelForUrls([request.url]);
          continue;
        }

        // If element exists but is not visible, mark as "fire and forget"
        // This will still cache on server but won't set on element
        // When user scrolls back, it will be cached and load quickly
        if (!this.isElementVisible(request.thumbnailDiv)) {
          request.fireAndForget = true;
        } else {
          // Element is visible again - clear fire and forget flag
          // This allows it to be processed normally when it gets its turn
          request.fireAndForget = false;
        }
      }
    }, 500); // Check every 500ms
  }

  /**
   * Start the queue processor
   */
  private startProcessor(): void {
    if (this.processorInterval) return;

    // Process queue every 100ms
    this.processorInterval = setInterval(() => {
      this.processQueue();
    }, 100);
  }

  /**
   * Stop the queue processor
   */
  public stop(): void {
    if (this.processorInterval) {
      clearInterval(this.processorInterval);
      this.processorInterval = null;
    }
    if (this.visibilityCheckInterval) {
      clearInterval(this.visibilityCheckInterval);
      this.visibilityCheckInterval = null;
    }
  }

  /**
   * Start the queue processors
   */
  public start(): void {
    this.startProcessor();
    this.startVisibilityChecker();
  }

  /**
   * Process the queue - pop from end (LIFO) and start requests
   */
  private processQueue(): void {
    if (this.lock || this.processing) return;
    if (this.pendingStack.length === 0) return;
    if (this.activeRequests.size >= this.maxConcurrent) return;

    this.processing = true;
    this.lock = true;

    try {
      const slotsAvailable = this.maxConcurrent - this.activeRequests.size;
      let processed = 0;

      // Separate visible and fire-and-forget requests for priority processing
      const visibleRequests: ThumbnailRequest[] = [];
      const fireAndForgetRequests: ThumbnailRequest[] = [];
      const otherRequests: ThumbnailRequest[] = [];

      // First pass: categorize requests by priority
      while (this.pendingStack.length > 0) {
        const request = this.pendingStack.pop();
        if (!request) break;

        // Check if element exists - if not, skip (element was removed)
        if (!request.thumbnailDiv || !request.imgElement) {
          continue;
        }

        // Check if retry is allowed
        const now = Date.now();
        if (request.retryCount > 0 && now < request.nextRetryTime) {
          otherRequests.push(request);
          continue;
        }

        // Check deduplication
        if (this.activeRequests.has(request.url) || this.completedRequests.has(request.url)) {
          continue;
        }

        // Categorize by priority: visible > fire-and-forget > others
        if (this.isElementVisible(request.thumbnailDiv) && !request.fireAndForget) {
          visibleRequests.push(request);
        } else if (request.fireAndForget) {
          fireAndForgetRequests.push(request);
        } else {
          otherRequests.push(request);
        }
      }

      // Process visible requests first (highest priority)
      while (
        processed < slotsAvailable &&
        visibleRequests.length > 0 &&
        this.activeRequests.size < this.maxConcurrent
      ) {
        const request = visibleRequests.pop();
        if (!request) break;
        this.startRequest(request);
        processed++;
      }

      // Then process fire-and-forget requests, but limit to max 2 concurrent
      // (reserve at least 3 slots for visible requests)
      const maxFireAndForget = Math.min(2, this.maxConcurrent - this.activeRequests.size);
      let fireAndForgetProcessed = 0;

      while (
        processed < slotsAvailable &&
        fireAndForgetProcessed < maxFireAndForget &&
        fireAndForgetRequests.length > 0 &&
        this.activeRequests.size < this.maxConcurrent
      ) {
        const request = fireAndForgetRequests.pop();
        if (!request) break;
        this.startRequest(request);
        processed++;
        fireAndForgetProcessed++;
      }

      // Put remaining requests back in stack (maintain priority order)
      // Visible first (highest priority), then others, then fire-and-forget (lowest priority)
      for (let i = fireAndForgetRequests.length - 1; i >= 0; i--) {
        this.pendingStack.push(fireAndForgetRequests[i]);
      }
      for (let i = otherRequests.length - 1; i >= 0; i--) {
        this.pendingStack.push(otherRequests[i]);
      }
      for (let i = visibleRequests.length - 1; i >= 0; i--) {
        this.pendingStack.push(visibleRequests[i]);
      }
    } finally {
      this.lock = false;
      this.processing = false;
    }
  }

  /**
   * Start an actual HTTP request for a thumbnail
   */
  private startRequest(request: ThumbnailRequest): void {
    // Mark as active
    this.activeRequests.set(request.url, request);

    // Create preload image
    const preloadImg = new Image();
    let completed = false;

    const cleanup = (success: boolean) => {
      if (completed) return;
      completed = true;

      // Remove from active requests
      this.activeRequests.delete(request.url);

      if (success) {
        // Mark as completed for deduplication
        this.completedRequests.add(request.url);

        // Clean up completed requests set to avoid unbounded growth (keep last 250)
        if (this.completedRequests.size > 400) {
          const entries = Array.from(this.completedRequests);
          this.completedRequests = new Set(entries.slice(-250));
        }
      }

      // Call callback
      if (request.callback) {
        request.callback(success, request.url);
      }
    };

    preloadImg.onload = () => {
      // Preload successful

      // If this is a "fire and forget" request, just mark as completed and cleanup
      // The server has cached it, which is all we need
      if (request.fireAndForget) {
        // Server has cached the thumbnail - mark as completed for deduplication
        // When user scrolls back, it will load from cache quickly
        cleanup(true);
        return;
      }

      // Normal request - set src on actual image element
      if (request.imgElement && request.imgElement.dataset.src === request.url) {
        const img = request.imgElement;

        // Set up handlers before setting src
        img.onload = () => {
          // Successfully loaded
          img.onload = null;
          img.onerror = null;

          // Mark as loaded
          img.setAttribute('data-loaded-at', Date.now().toString());
          if (request.thumbnailDiv) {
            const placeholder = request.thumbnailDiv.querySelector('.thumbnail-placeholder') as HTMLElement;
            if (placeholder) {
              placeholder.style.display = 'none';
            }
            const errorIndicator = request.thumbnailDiv.querySelector('.thumbnail-error') as HTMLElement;
            if (errorIndicator) {
              errorIndicator.style.display = 'none';
            }
          }

          cleanup(true);
        };

        img.onerror = () => {
          // Failed to load
          const error = new Error(`Thumbnail image element failed to load: ${request.url}`);
          errorLogger.logThumbnailError(request.url, error, {
            groupId: request.groupId,
            memberIndex: request.memberIndex,
            phase: 'destination',
          });

          img.onerror = null;
          img.onload = null;

          if (request.thumbnailDiv) {
            img.style.display = 'none';
            const placeholder = request.thumbnailDiv.querySelector('.thumbnail-placeholder') as HTMLElement;
            if (placeholder) {
              placeholder.style.display = 'none';
            }
            const errorIndicator = request.thumbnailDiv.querySelector('.thumbnail-error') as HTMLElement;
            if (errorIndicator) {
              errorIndicator.style.display = 'flex';
            }
          }

          cleanup(false);
        };

        // Set src to trigger load
        img.src = request.url;
        img.removeAttribute('data-src');
        img.style.display = '';

        // Check if already cached
        if (img.complete && img.naturalHeight !== 0) {
          if (img.onload) {
            img.onload(new Event('load'));
          }
        }
      } else {
        // Element no longer exists or doesn't match
        cleanup(false);
      }
    };

    preloadImg.onerror = () => {
      // Preload failed - schedule retry if allowed
      if (request.retryCount < this.MAX_RETRIES) {
        request.retryCount++;

        // Exponential backoff: INITIAL_RETRY_DELAY * 2^(retryCount - 1)
        const delay = Math.min(
          this.INITIAL_RETRY_DELAY * Math.pow(2, request.retryCount - 1),
          this.MAX_RETRY_DELAY
        );
        request.nextRetryTime = Date.now() + delay;

        // Log retry attempt
        const error = new Error(`Thumbnail preload failed (retry ${request.retryCount}/${this.MAX_RETRIES}): ${request.url}`);
        errorLogger.logThumbnailError(request.url, error, {
          groupId: request.groupId,
          memberIndex: request.memberIndex,
          phase: 'preload',
          retryCount: request.retryCount,
          nextRetryTime: request.nextRetryTime,
        });

        // Add back to queue for retry (at end, so it's processed after new requests)
        this.pendingStack.push(request);
      } else {
        // Max retries exceeded - fail
        const error = new Error(`Thumbnail preload failed after ${this.MAX_RETRIES} retries: ${request.url}`);
        errorLogger.logThumbnailError(request.url, error, {
          groupId: request.groupId,
          memberIndex: request.memberIndex,
          phase: 'preload',
          retryCount: request.retryCount,
          finalFailure: true,
        });

        if (request.thumbnailDiv) {
          const placeholder = request.thumbnailDiv.querySelector('.thumbnail-placeholder') as HTMLElement;
          if (placeholder) {
            placeholder.style.display = 'none';
          }
          const errorIndicator = request.thumbnailDiv.querySelector('.thumbnail-error') as HTMLElement;
          if (errorIndicator) {
            errorIndicator.style.display = 'flex';
          }
        }
        cleanup(false);
      }
    };

    // Start loading
    try {
      preloadImg.src = request.url;
    } catch (err) {
      // Immediate failure (invalid URL, etc.)
      const error = err instanceof Error ? err : new Error(String(err));
      errorLogger.logThumbnailError(request.url, error, {
        groupId: request.groupId,
        memberIndex: request.memberIndex,
        phase: 'setup',
      });
      cleanup(false);
    }
  }

  /**
   * Get current queue stats
   */
  public getStats() {
    return {
      pending: this.pendingStack.length,
      active: this.activeRequests.size,
      completed: this.completedRequests.size,
      maxConcurrent: this.maxConcurrent,
    };
  }

  /**
   * Get active request URLs
   */
  public getActiveRequests(): string[] {
    return Array.from(this.activeRequests.keys());
  }
}

// Singleton instance
export const thumbnailQueue = new ThumbnailRequestQueue();

// Expose for debugging
if (typeof window !== 'undefined') {
  window.thumbnailQueue = thumbnailQueue;
}

if (typeof window !== 'undefined' && window.electronAPI) {
  if (window.electronAPI.onSuspend) {
    window.electronAPI.onSuspend(() => {
      console.log('[ThumbnailQueue] System suspended, stopping processor');
      thumbnailQueue.stop();
    });
  }
  if (window.electronAPI.onResume) {
    window.electronAPI.onResume(() => {
      console.log('[ThumbnailQueue] System resumed, starting processor');
      thumbnailQueue.start();
    });
  }
}
