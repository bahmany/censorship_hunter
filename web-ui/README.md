# Hunter Web UI

A Progressive Web Application (PWA) for managing Hunter proxy configuration discovery system with offline capabilities.

## Features

- **Offline Support**: Works without internet connection using service worker caching
- **PWA Capabilities**: Installable as a desktop/mobile app
- **Config Download**: Download gold, silver, or all configurations
- **Real-time Status**: WebSocket connection to Hunter backend
- **Responsive Design**: Works on desktop and mobile devices
- **Dark Theme**: Built-in dark mode for comfortable viewing

## Installation

### Prerequisites

- Node.js 18+ 
- npm or yarn

### Setup

1. Install dependencies:
```bash
npm install
```

2. Run development server:
```bash
npm run dev
```

3. Build for production:
```bash
npm run build
```

4. Start production server:
```bash
npm start
```

## PWA Installation

### Desktop (Chrome/Edge)
1. Open the app in Chrome or Edge
2. Click the install icon in the address bar
3. Follow the prompts to install

### Mobile (Android/iOS)
1. Open the app in Chrome (Android) or Safari (iOS)
2. Tap "Add to Home Screen" or "Share" > "Add to Home Screen"
3. Follow the prompts

## Offline Usage

The app caches static assets and data for offline use:
- Static assets are cached for offline viewing
- WebSocket connections require internet connectivity
- Download functionality requires internet connection
- Status data is cached for offline viewing

## Configuration

### Backend Connection

The app connects to the Hunter backend via WebSocket:
- **Status WebSocket**: `ws://<hostname>:7802`
- **Command WebSocket**: `ws://<hostname>:7801`
- **API Endpoint**: `http://<hostname>:7800/api/configs/<type>`

Connection settings can be configured in the Settings tab.

## Development

### Project Structure

```
web-ui/
├── app/
│   ├── globals.css       # Global styles
│   ├── layout.tsx        # Root layout with PWA metadata
│   └── page.tsx          # Main application page
├── public/
│   ├── manifest.json     # PWA manifest
│   ├── sw.js            # Custom service worker
│   ├── icon-192.png     # App icon (192x192)
│   └── icon-512.png     # App icon (512x512)
├── package.json         # Dependencies
├── next.config.js       # Next.js config with PWA
├── tailwind.config.ts   # Tailwind CSS config
└── tsconfig.json        # TypeScript config
```

### Key Technologies

- **Next.js 14**: React framework
- **React 18**: UI library
- **TypeScript**: Type safety
- **Tailwind CSS**: Styling
- **Lucide React**: Icons
- **next-pwa**: PWA support
- **file-saver**: File download functionality

## Features by Tab

### Home
- Real-time status display
- Control panel (Start/Stop/Pause/Resume)
- Proxy endpoint information
- Connection status indicators

### Configs
- Configuration database statistics
- Download gold/silver/all configs
- Offline status warnings

### Statistics
- Performance metrics
- Download status reports
- Data refresh functionality

### Settings
- Application settings
- PWA settings (offline mode, background sync)
- Connection configuration
- Save/reset options

## Troubleshooting

### Service Worker Issues

If the PWA doesn't update after deployment:
1. Clear browser cache
2. Unregister old service workers in DevTools
3. Hard refresh the page

### WebSocket Connection

If connection fails:
1. Verify backend is running
2. Check hostname and port settings
3. Ensure firewall allows WebSocket connections

### Build Errors

If build fails:
1. Clear Next.js cache: `rm -rf .next`
2. Reinstall dependencies: `rm -rf node_modules && npm install`
3. Check Node.js version (requires 18+)

## License

See LICENSE file in parent directory.
