// Module-level tracking for active thumbnail requests across all components
// This module now delegates to the thumbnail queue system

import { thumbnailQueue } from './thumbnailQueue';

/**
 * @deprecated Use thumbnailQueue.push() directly instead
 * This is kept for backward compatibility
 */
export function addActiveRequest(_url: string): void {
  // No-op - queue handles tracking
}

/**
 * @deprecated Use thumbnailQueue.cancelForUrls() directly instead
 * This is kept for backward compatibility
 */
export function removeActiveRequest(_url: string): void {
  // No-op - queue handles tracking
}

/**
 * Get the number of active (in-flight) thumbnail requests
 */
export function getActiveRequestCount(): number {
  return thumbnailQueue.getStats().active;
}

/**
 * Clear all pending requests (not active ones)
 */
export function clearAllRequests(): void {
  thumbnailQueue.cancelAll();
}

/**
 * Get list of active request URLs
 */
export function getActiveRequests(): string[] {
  return thumbnailQueue.getActiveRequests();
}
