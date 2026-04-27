// Error logging utility for capturing and persisting errors
import type { ErrorLogEntry } from '../types';

const ERROR_LOG_KEY = 'dedup_app_error_log';
const MAX_ERRORS = 500; // Keep last 500 errors
const MAX_ERROR_AGE_DAYS = 7; // Keep errors for 7 days

class ErrorLogger {
  private errorLog: ErrorLogEntry[] = [];
  private isInitialized = false;
  private getActiveRequestCount?: () => number;

  constructor() {
    this.loadFromStorage();
    this.initialize();
  }

  private loadFromStorage(): void {
    try {
      const stored = localStorage.getItem(ERROR_LOG_KEY);
      if (stored) {
        this.errorLog = JSON.parse(stored);
        // Clean up old errors
        this.cleanupOldErrors();
      }
    } catch (e) {
      console.warn('Failed to load error log from storage:', e);
      this.errorLog = [];
    }
  }

  private saveToStorage(): void {
    try {
      // Keep only last MAX_ERRORS entries
      if (this.errorLog.length > MAX_ERRORS) {
        this.errorLog = this.errorLog.slice(-MAX_ERRORS);
      }
      localStorage.setItem(ERROR_LOG_KEY, JSON.stringify(this.errorLog));
    } catch (e) {
      console.warn('Failed to save error log to storage:', e);
    }
  }

  private cleanupOldErrors(): void {
    const cutoffTime = Date.now() - (MAX_ERROR_AGE_DAYS * 24 * 60 * 60 * 1000);
    this.errorLog = this.errorLog.filter(entry => {
      const entryTime = new Date(entry.timestamp).getTime();
      return entryTime > cutoffTime;
    });
    this.saveToStorage();
  }

  private initialize(): void {
    if (this.isInitialized) return;
    this.isInitialized = true;

    // Capture console.error
    const originalConsoleError = console.error;
    console.error = (...args: unknown[]) => {
      const message = args.map(arg => 
        typeof arg === 'object' ? JSON.stringify(arg, null, 2) : String(arg)
      ).join(' ');
      
      this.logError({
        type: 'error',
        message,
        stack: new Error().stack,
      });
      
      originalConsoleError.apply(console, args);
    };

    // Global error handler
    window.addEventListener('error', (event) => {
      this.logError({
        type: 'error',
        message: event.message || 'Unknown error',
        source: event.filename,
        lineno: event.lineno,
        colno: event.colno,
        stack: event.error?.stack,
        url: window.location.href,
      });
    });

    // Unhandled promise rejection handler
    window.addEventListener('unhandledrejection', (event) => {
      const reason = event.reason;
      const message = reason instanceof Error 
        ? reason.message 
        : String(reason || 'Unhandled promise rejection');
      const stack = reason instanceof Error ? reason.stack : undefined;

      this.logError({
        type: 'unhandledRejection',
        message,
        stack,
        context: {
          reason: reason instanceof Error ? reason.toString() : reason,
        },
      });
    });
  }

  public logError(entry: Omit<ErrorLogEntry, 'timestamp'>): void {
    const fullEntry: ErrorLogEntry = {
      ...entry,
      timestamp: new Date().toISOString(),
    };

    this.errorLog.push(fullEntry);
    
    // Keep log size manageable
    if (this.errorLog.length > MAX_ERRORS) {
      this.errorLog.shift();
    }

    this.saveToStorage();
  }

  public logThumbnailError(
    url: string,
    error: Error | string,
    context?: Record<string, unknown>
  ): void {
    const message = error instanceof Error ? error.message : String(error);
    const stack = error instanceof Error ? error.stack : undefined;

    this.logError({
      type: 'thumbnail',
      message: `Thumbnail load failed: ${message}`,
      url,
      stack,
      context: {
        ...context,
        activeRequests: this.getActiveRequestCount?.() || 'unknown',
      },
    });
  }

  public getErrors(limit?: number): ErrorLogEntry[] {
    const errors = [...this.errorLog];
    if (limit) {
      return errors.slice(-limit);
    }
    return errors;
  }

  public getErrorCount(): number {
    return this.errorLog.length;
  }

  public clearErrors(): void {
    this.errorLog = [];
    this.saveToStorage();
  }

  public exportErrors(): string {
    return JSON.stringify(this.errorLog, null, 2);
  }

  public exportErrorsAsText(): string {
    return this.errorLog.map(entry => {
      const lines = [
        `[${entry.timestamp}] ${entry.type.toUpperCase()}: ${entry.message}`,
      ];
      
      if (entry.url) {
        lines.push(`  URL: ${entry.url}`);
      }
      
      if (entry.source) {
        lines.push(`  Source: ${entry.source}:${entry.lineno}:${entry.colno}`);
      }
      
      if (entry.stack) {
        lines.push(`  Stack:\n${entry.stack.split('\n').map(l => `    ${l}`).join('\n')}`);
      }
      
      if (entry.context) {
        lines.push(`  Context: ${JSON.stringify(entry.context, null, 2)}`);
      }
      
      return lines.join('\n');
    }).join('\n\n');
  }
  
  public setActiveRequestCountGetter(getter: () => number): void {
    this.getActiveRequestCount = getter;
  }
}

// Singleton instance
export const errorLogger = new ErrorLogger();

// Initialize error logger with active request count getter
// Use setTimeout to ensure thumbnailRequests module is fully loaded
if (typeof window !== 'undefined') {
  setTimeout(() => {
    import('./thumbnailRequests').then(({ getActiveRequestCount }) => {
      errorLogger.setActiveRequestCountGetter(getActiveRequestCount);
    }).catch(err => {
      console.warn('Failed to initialize error logger with request count getter:', err);
    });
  }, 0);
}

// Expose to window for debugging
if (typeof window !== 'undefined') {
  (window as unknown as { errorLogger: ErrorLogger }).errorLogger = errorLogger;
  window.exportErrors = () => errorLogger.exportErrorsAsText();
  window.getErrorCount = () => errorLogger.getErrorCount();
  window.clearErrors = () => errorLogger.clearErrors();
}
