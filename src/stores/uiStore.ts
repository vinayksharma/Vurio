import { create } from 'zustand';
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

export const useUIStore = create<UIState>((set) => ({
  activePanel: 'duplicates',
  sidebarSize: 260,
  sidebarHeight: undefined,
  isVertical: false,
  sidebarOpen: false,
  debugOverlayEnabled: false,

  setActivePanel: (panel) => set({ activePanel: panel }),
  setSidebarSize: (size) => set({ sidebarSize: size }),
  setSidebarHeight: (height) => set({ sidebarHeight: height }),
  setIsVertical: (isVertical) => set({ isVertical }),
  setSidebarOpen: (open) => set({ sidebarOpen: open }),
  toggleSidebar: () => set((state) => ({ sidebarOpen: !state.sidebarOpen })),
  setDebugOverlayEnabled: (enabled) => set({ debugOverlayEnabled: enabled }),
}));
