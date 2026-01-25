# VURIO (Bun/TypeScript/React)

A desktop application built with Electron, React, TypeScript, and **Bun**.

## Features

- **Media Locations Panel** - Register and manage directories for scanning
- **Duplicates Panel** - View duplicate groups with virtualized scrolling and thumbnail previews
- **Settings Panel** - Live configuration editing with validation
- **Advanced Panel** - Maintenance tools and debugging utilities
- **Status Bar** - Real-time server status monitoring

## Tech Stack

- **Runtime**: Bun (primary), Node.js compatible
- **Frontend**: React 18 with TypeScript
- **State Management**: Zustand (lightweight, performant)
- **Desktop**: Electron
- **Build Tool**: Vite (via Bun), Bun bundler for Electron
- **Styling**: CSS with macOS HIG design guidelines

## Prerequisites

- **Bun 1.0+** (recommended) - Install from [bun.sh](https://bun.sh)
- Or Node.js 18+ with npm (fallback)
- Dedup backend server running on `http://localhost:8088`

## Installation

```bash
# Using Bun (recommended)
bun install

# Or using npm (fallback)
npm install
```

## Development

### With Bun (recommended)

```bash
# Start Vite dev server
bun run bun:dev

# Start with Electron
bun run bun:dev:electron

# Access at http://localhost:5173
```

### With npm (fallback)

```bash
# Start Vite dev server
npm run dev

# Start with Electron
npm run dev:electron
```

### Standalone UI Server

```bash
# Build first
bun run bun:build  # or: npm run build

# Start the server (uses Bun if available, Node.js fallback)
bun run bun:serve  # or: npm run serve

# Access at http://localhost:1212
```

## Building

### With Bun (recommended)

```bash
# Build using Bun bundler for Electron files
bun run bun:build
```

### With npm (fallback)

```bash
# Build using esbuild for Electron files
npm run build
```

This creates:
- `dist/` - Built React application (via Vite)
- `dist-electron/` - Built Electron main/preload scripts (Bun bundler or esbuild)

## Running in Production

### As Electron App

```bash
bun run bun:build && bun run electron
# or
npm run build && npm run electron
```

### As Web App (Browser)

```bash
bun run bun:build && bun run bun:serve
# or
npm run build && npm run serve
# Access at http://localhost:1212
```

## Project Structure

```
dedup_bun_app/
├── electron/           # Electron main process
│   ├── main.ts        # Main process entry
│   └── preload.ts     # Preload script for IPC
├── src/
│   ├── components/    # React components
│   ├── stores/        # Zustand state stores
│   ├── types/         # TypeScript type definitions
│   ├── utils/         # Utility functions
│   ├── App.tsx        # Main App component
│   ├── main.tsx       # React entry point
│   ├── index.html     # HTML template
│   └── index.css      # Global styles
├── server.ts          # Bun/Node UI server
├── vite.config.ts     # Vite configuration
├── tsconfig.json      # TypeScript configuration
└── package.json       # Project dependencies
```

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `UI_HOST` | `127.0.0.1` | Server host |
| `UI_PORT` | `1212` | Server port (UI server) / `5173` (Vite) |
| `API_BASE` | `http://localhost:8088` | Backend API URL |
| `NODE_ENV` | `development` | Environment mode |

## API Endpoints

The app proxies all `/api/*` requests to the backend server. Key endpoints:

- `GET /api/v1/server/status` - Server status
- `GET /api/v1/duplicates/groups` - Fetch duplicate groups
- `POST /api/v1/media-locations/register` - Register a directory
- `POST /api/v1/media-locations/deregister` - Deregister a directory
- `GET /api/v1/config` - Get configuration
- `PUT /api/v1/config/:key` - Update configuration
- `GET /api/v1/thumbnails` - Get thumbnail images

## State Management

Uses Zustand for efficient, lightweight state management:

- `useServerStatusStore` - Server connection status
- `useUIStore` - UI state (panels, sidebar, layout)
- `useImagePopupStore` - Image preview modal
- `useStatusPopupStore` - Status/result modals

## License

MIT
