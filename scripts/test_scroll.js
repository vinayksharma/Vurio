/**
 * Long-running scroll test for duplicates panel
 * Run this in the browser console (F12 or Cmd+Option+I)
 * 
 * Alternatively, use the built-in scroll test:
 *   window.startScrollTest()  - Start test
 *   window.stopScrollTest()   - Stop test
 */
(async function scrollTest() {
  console.log('[ScrollTest] Starting long-running scroll test...');

  // Ensure duplicates panel is active
  const duplicatesBtn = Array.from(document.querySelectorAll('button')).find(b =>
    b.textContent.includes('Duplicates') || b.textContent.includes('🫂')
  );
  if (duplicatesBtn) {
    duplicatesBtn.click();
    console.log('[ScrollTest] Clicked duplicates button');
    await new Promise(resolve => setTimeout(resolve, 2000));
  }

  // Find scroll container
  const scrollContainer = document.getElementById('duplicates-stack');
  if (!scrollContainer) {
    console.error('[ScrollTest] Could not find duplicates-stack container');
    return { error: 'Container not found' };
  }

  // Get total groups from meta
  const metaElement = document.getElementById('duplicates-meta');
  const totalText = metaElement?.textContent || '';
  const totalMatch = totalText.match(/(\d+)/);
  const totalGroups = totalMatch ? parseInt(totalMatch[1]) : 0;

  console.log(`[ScrollTest] Found ${totalGroups} total groups`);

  if (totalGroups === 0) {
    console.error('[ScrollTest] No groups found');
    return { error: 'No groups found' };
  }

  // Focus the scroll container
  scrollContainer.focus();

  const viewportHeight = scrollContainer.clientHeight;
  let scrollPosition = 0;
  let step = 0;
  const maxScroll = scrollContainer.scrollHeight - scrollContainer.clientHeight;

  console.log(`[ScrollTest] Starting scroll test. Viewport: ${viewportHeight}px, Max scroll: ${maxScroll}px`);
  console.log(`[ScrollTest] Estimated steps: ${Math.ceil(maxScroll / viewportHeight)}`);

  // Scroll one page at a time, waiting 3 seconds between each
  while (scrollPosition < maxScroll - 10) {
    step++;
    const currentPercent = Math.round((scrollPosition / maxScroll) * 100);
    console.log(`[ScrollTest] Step ${step}: Scrolling to ${scrollPosition}px (${currentPercent}%)`);

    // Scroll to position
    scrollContainer.scrollTop = scrollPosition;

    // Wait 3 seconds
    await new Promise(resolve => setTimeout(resolve, 3000));

    // Check if we've reached the end
    const actualScroll = scrollContainer.scrollTop;
    if (actualScroll >= maxScroll - 10) {
      console.log('[ScrollTest] Reached end of scroll');
      break;
    }

    // Check queue stats for monitoring
    if (window.thumbnailQueue) {
      const stats = window.thumbnailQueue.getStats();
      console.log(`[ScrollTest] Queue stats - P:${stats.pending} A:${stats.active} C:${stats.completed}`);
    }

    // Move to next page
    scrollPosition += viewportHeight;

    // Safety check - don't run forever
    if (step > 2000) {
      console.log('[ScrollTest] Safety limit reached (2000 steps)');
      break;
    }
  }

  console.log(`[ScrollTest] Test completed after ${step} steps`);
  console.log(`[ScrollTest] Final scroll position: ${scrollContainer.scrollTop}px / ${maxScroll}px`);

  return {
    success: true,
    steps: step,
    totalGroups,
    finalPosition: scrollContainer.scrollTop,
    maxScroll: maxScroll
  };
})();
