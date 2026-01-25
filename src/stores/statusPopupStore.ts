import { create } from 'zustand';
import type { ChangePathResponse } from '../types';

interface StatusPopupState {
  isOpen: boolean;
  result: ChangePathResponse | null;
  oldPath: string;
  newPath: string;
  verified: boolean;
  open: (result: ChangePathResponse, oldPath: string, newPath: string, verified: boolean) => void;
  close: () => void;
}

export const useStatusPopupStore = create<StatusPopupState>((set) => ({
  isOpen: false,
  result: null,
  oldPath: '',
  newPath: '',
  verified: false,

  open: (result, oldPath, newPath, verified) => set({
    isOpen: true,
    result,
    oldPath,
    newPath,
    verified,
  }),

  close: () => set({
    isOpen: false,
  }),
}));
