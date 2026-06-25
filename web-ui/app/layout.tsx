import './globals.css'
import type { Metadata, Viewport } from 'next'
import { ThemeProvider } from '@mui/material/styles'
import CssBaseline from '@mui/material/CssBaseline'
import { darkTheme } from './theme'

export const metadata: Metadata = {
  title: 'Hunter - Proxy Configuration Discovery',
  description: 'Autonomous proxy configuration discovery and load balancing',
  icons: {
    icon: 'icon-192.png',
    apple: 'icon-192.png',
  },
  appleWebApp: {
    capable: true,
    statusBarStyle: 'default',
    title: 'Hunter',
  },
}

export const viewport: Viewport = {
  themeColor: '#0f172a',
}

export default function RootLayout({
  children,
}: {
  children: React.ReactNode
}) {
  return (
    <html lang="en">
      <body>
        <ThemeProvider theme={darkTheme}>
          <CssBaseline />
          {children}
        </ThemeProvider>
      </body>
    </html>
  )
}
