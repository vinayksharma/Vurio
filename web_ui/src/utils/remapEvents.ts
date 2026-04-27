/**
 * Global event system for remap operations
 * Allows components to listen for remap events and invalidate caches
 */

import type { RemapEvent } from '../types';

type RemapEventListener = (event: RemapEvent) => void;

class RemapEventEmitter {
  private listeners: Set<RemapEventListener> = new Set();

  /**
   * Subscribe to remap events
   * @param listener Callback function to be called when a remap occurs
   * @returns Unsubscribe function
   */
  subscribe(listener: RemapEventListener): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  /**
   * Emit a remap event to all subscribers
   */
  emit(event: RemapEvent): void {
    console.log(`[RemapEvent] Emitting remap event: ${event.oldPath} -> ${event.newPath}`);
    this.listeners.forEach((listener) => {
      try {
        listener(event);
      } catch (error) {
        console.error('[RemapEvent] Error in listener:', error);
      }
    });
  }

  /**
   * Get the number of active listeners
   */
  getListenerCount(): number {
    return this.listeners.size;
  }
}

// Singleton instance
export const remapEventEmitter = new RemapEventEmitter();

// Expose to window for debugging
if (typeof window !== 'undefined') {
  (window as unknown as { remapEventEmitter: RemapEventEmitter }).remapEventEmitter = remapEventEmitter;
}

export type { RemapEvent };
