import { create } from 'zustand';

interface ImagePopupState {
  isOpen: boolean;
  filePath: string;
  details: string;
  open: (options: { filePath: string; details: string }) => void;
  close: () => void;
}

export const useImagePopupStore = create<ImagePopupState>((set) => ({
  isOpen: false,
  filePath: '',
  details: '',

  open: ({ filePath, details }) => set({
    isOpen: true,
    filePath,
    details,
  }),

  close: () => set({
    isOpen: false,
  }),
}));

// Expose to window for compatibility with original code
if (typeof window !== 'undefined') {
  window.imagePopup = {
    open: (options: { filePath: string; details: string }) => {
      useImagePopupStore.getState().open(options);
    },
    close: () => {
      useImagePopupStore.getState().close();
    },
  };
}
