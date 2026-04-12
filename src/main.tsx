import React from 'react';
import ReactDOM from 'react-dom/client';
import App from './App';
import ImagePopup from './components/ImagePopup';
import './index.css';
import './utils/errorLogger'; // Initialize error logging
import './utils/thumbnailQueue'; // Initialize thumbnail queue

const params = new URLSearchParams(window.location.search);
const isViewer = params.get('window') === 'viewer';

let RootComponent = isViewer ? ImagePopup : App;

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <RootComponent />
  </React.StrictMode>
);
