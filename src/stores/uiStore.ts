import { create } from 'zustand';
import { persist, devtools } from 'zustand/middleware';
import type { PanelType } from '../types';

interface UIState {
  activePanel: PanelType;
  sidebarSize: number;
  sidebarHeight: number | undefined;
  isVertical: boolean;
  sidebarOpen: boolean;
  debugOverlayEnabled: boolean;
  setActivePanel: (panel: PanelType) => void;
  setSidebarSize: (size: number) => void;
  setSidebarHeight: (height: number | undefined) => void;
  setIsVertical: (isVertical: boolean) => void;
  setSidebarOpen: (open: boolean) => void;
  toggleSidebar: () => void;
  setDebugOverlayEnabled: (enabled: boolean) => void;
}

export const useUIStore = create<UIState>()(
  devtools(
    persist(
      (set) => ({
        activePanel: 'duplicates',
        sidebarSize: 260,
        sidebarHeight: undefined,
        isVertical: false,
        sidebarOpen: true, // Default to true for better visibility
        debugOverlayEnabled: false,

        setActivePanel: (panel) => set({ activePanel: panel }, false, 'ui/setActivePanel'),
        setSidebarSize: (size) => set({ sidebarSize: size }, false, 'ui/setSidebarSize'),
        setSidebarHeight: (height) => set({ sidebarHeight: height }, false, 'ui/setSidebarHeight'),
        setIsVertical: (isVertical) => set({ isVertical }, false, 'ui/setIsVertical'),
        setSidebarOpen: (open) => set({ sidebarOpen: open }, false, 'ui/setSidebarOpen'),
        toggleSidebar: () => set((state) => ({ sidebarOpen: !state.sidebarOpen }), false, 'ui/toggleSidebar'),
        setDebugOverlayEnabled: (enabled) => set({ debugOverlayEnabled: enabled }, false, 'ui/setDebugOverlayEnabled'),
      }),
      {
        name: 'vurio-ui-storage',
        partialize: (state) => ({
          activePanel: state.activePanel,
          sidebarSize: state.sidebarSize,
          isVertical: state.isVertical,
          sidebarOpen: state.sidebarOpen,
        }),
      }
    ),
    { name: 'VurioUIStore' }
  )
);
