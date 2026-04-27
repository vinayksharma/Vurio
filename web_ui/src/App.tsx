import { useEffect } from 'react';
import SidebarControl from './components/SidebarControl';
import ContentAreaControl from './components/ContentAreaControl';
import Splitter from './components/Splitter';
import ImagePopup from './components/ImagePopup';
import { useUIStore, useServerStatusStore } from './stores';
import { fetchUserSetting } from './utils/api';
import './index.css';

function App() {
  const {
    activePanel,
    setActivePanel,
    sidebarSize,
    setSidebarSize,
    sidebarHeight,
    setSidebarHeight,
    isVertical,
    setIsVertical,
    sidebarOpen,
    setSidebarOpen,
    toggleSidebar,
  } = useUIStore();

  const { startPolling, stopPolling } = useServerStatusStore();

  // Start server status polling on mount
  useEffect(() => {
    startPolling();
    return () => stopPolling();
  }, [startPolling, stopPolling]);

  // Load UI theme on startup
  useEffect(() => {
    fetchUserSetting('ui_theme')
      .then((theme) => {
        if (theme && theme !== 'os') {
          document.documentElement.setAttribute('data-theme', theme);
        }
      })
      .catch((err) => console.error('Failed to load UI theme on startup:', err));
  }, []);

  useEffect(() => {
    const checkLayout = () => {
      const shouldBeVertical = window.innerWidth < 768;
      setIsVertical(shouldBeVertical);
      
      if (shouldBeVertical) {
        // In vertical mode, calculate initial height for sidebar
        const availableHeight = window.innerHeight - 28; // minus titlebar
        if (sidebarHeight === undefined) {
          setSidebarHeight(Math.floor(availableHeight / 2));
        }
      } else {
        setSidebarHeight(undefined);
      }
    };

    checkLayout();
    window.addEventListener('resize', checkLayout);
    return () => window.removeEventListener('resize', checkLayout);
  }, [sidebarHeight, setIsVertical, setSidebarHeight]);

  const handleSidebarResize = (size: number) => {
    if (isVertical) {
      setSidebarHeight(size);
    } else {
      setSidebarSize(size);
    }
  };

  return (
    <>
      <div className="titlebar-drag"></div>
      <div className={`app-shell ${isVertical ? 'vertical' : 'horizontal'}`}>
        {!isVertical && (
          <>
            <SidebarControl
              activePanel={activePanel}
              onPanelChange={setActivePanel}
              width={sidebarSize}
              isVertical={false}
            />
            <Splitter
              direction="horizontal"
              onResize={handleSidebarResize}
              initialSize={sidebarSize}
              minSize={150}
              maxSize={500}
            />
          </>
        )}
        <ContentAreaControl
          activePanel={activePanel}
          flex={isVertical ? undefined : 1}
          isVertical={isVertical}
          onHamburgerClick={toggleSidebar}
        />
      </div>
      {isVertical && (
        <SidebarControl
          activePanel={activePanel}
          onPanelChange={(panel) => {
            setActivePanel(panel);
            setSidebarOpen(false);
          }}
          isVertical={true}
          isOpen={sidebarOpen}
          onClose={() => setSidebarOpen(false)}
        />
      )}
    </>
  );
}

export default App;
