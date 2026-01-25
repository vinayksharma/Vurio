/**
 * Script to check error log from LocalStorage
 * This can be run in the browser console or via Bun if we have access to Electron's storage
 * 
 * Usage in browser console:
 *   Copy and paste this script, or run: checkErrorLog()
 * 
 * Usage with Bun:
 *   bun run scripts/check_error_log.ts
 */

const ERROR_LOG_KEY = 'dedup_app_error_log';

interface ErrorLogEntry {
  timestamp: string;
  type: 'error' | 'unhandledRejection' | 'thumbnail' | 'react';
  message: string;
  source?: string;
  lineno?: number;
  colno?: number;
  stack?: string;
  url?: string;
  context?: Record<string, unknown>;
}

function checkErrorLog() {
  // Check if we're in a browser context
  if (typeof localStorage !== 'undefined') {
    try {
      const stored = localStorage.getItem(ERROR_LOG_KEY);
      if (stored) {
        const errors: ErrorLogEntry[] = JSON.parse(stored);
        console.log(`Found ${errors.length} errors in log`);
        console.log('\n=== RECENT ERRORS (last 10) ===');
        errors.slice(-10).forEach((error, i) => {
          console.log(`\n[${i + 1}] ${error.timestamp}`);
          console.log(`Type: ${error.type}`);
          console.log(`Message: ${error.message}`);
          if (error.url) console.log(`URL: ${error.url}`);
          if (error.context) console.log(`Context:`, error.context);
          if (error.stack) console.log(`Stack: ${error.stack.split('\n').slice(0, 3).join('\n')}`);
        });
        return errors;
      } else {
        console.log('No errors found in log');
        return [];
      }
    } catch (e) {
      console.error('Error reading log:', e);
      return [];
    }
  } else {
    console.log('localStorage not available - run this in browser console');
    console.log('\nAlternatively, use these commands in the browser DevTools console:');
    console.log('  window.exportErrors()     - Export all errors as text');
    console.log('  window.getErrorCount()    - Get total error count');
    console.log('  window.clearErrors()      - Clear all errors');
    return [];
  }
}

// Export for use as a module
export { checkErrorLog, ERROR_LOG_KEY };
export type { ErrorLogEntry };

// Run if executed directly
if (typeof window === 'undefined') {
  console.log('This script should be run in a browser context.');
  console.log('\nTo check errors in the Dedup Desktop app:');
  console.log('1. Open the app');
  console.log('2. Open DevTools (Cmd+Option+I or via Advanced panel)');
  console.log('3. In the Console tab, run:');
  console.log('   window.exportErrors()     - Export all errors as text');
  console.log('   window.getErrorCount()    - Get total error count');
  console.log('   window.clearErrors()      - Clear all errors');
}
