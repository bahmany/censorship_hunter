'use client'

import { useEffect, useState, useRef, useCallback } from 'react'
import {
  AppBar, Toolbar, Typography, Box, Container, Grid, Card, CardContent,
  Tabs, Tab, Button, ButtonGroup, IconButton, Tooltip, Chip, Paper,
  Table, TableBody, TableCell, TableContainer, TableHead, TableRow,
  LinearProgress, Divider, TextField, MenuItem, FormControl, InputLabel,
  Select, Alert, Snackbar, Stack, Avatar, Badge, List, ListItem,
  ListItemText, ListItemIcon, Collapse, IconButton as MuiIconButton,
  ToggleButton, ToggleButtonGroup, Dialog, DialogTitle, DialogContent,
  DialogActions, DialogContentText, CircularProgress, TablePagination
} from '@mui/material'
import {
  PlayArrow, Stop, Pause, Refresh, Download, CloudUpload, Speed,
  Settings as SettingsIcon, Dashboard, Dns, People, Security,
  WifiOff, Wifi, Delete, Add, Search, Router, Memory, Bolt,
  TrendingUp, CheckCircle, Error as ErrorIcon, Warning, Info,
  Terminal, Build, Science, TravelExplore, RefreshOutlined,
  KeyboardArrowDown, KeyboardArrowUp, ContentCopy, Visibility,
  VisibilityOff, QrCode2, QrCodeScanner, OpenInFull, NetworkCheck,
  HealthAndSafety, Route, Hub, MonitorHeart, Speed as SpeedIcon,
  CloudDone, Assessment, Cloud, Widgets
} from '@mui/icons-material'
import { saveAs } from 'file-saver'
import QRCode from 'qrcode'

interface HunterStatus {
  ts: number
  ts_ms: number
  phase: string
  paused: boolean
  uptime_s: number
  balancer_backends: number
  pending_unique: number
  eta_seconds: number
  db: {
    total: number
    alive: number
    tested_unique: number
    untested_unique: number
    stale_unique: number
    avg_latency_ms: number
    total_tests: number
    total_passes: number
  }
  validator: {
    last_tested: number
    last_passed: number
    interval_s: number
    active_test_processes: number
    max_test_processes: number
    rate_per_s: number
  }
  speed: {
    profile: string
    max_threads: number
    test_timeout_s: number
    chunk_size: number
    effective_max_concurrent: number
    effective_timeout_s: number
    effective_batch_size: number
    effective_chunk_size: number
  }
  hardware: {
    cpu_count: number
    cpu_percent: number
    ram_total_gb: number
    ram_used_gb: number
    ram_percent: number
    mode: string
    io_pool_size: number
    cpu_pool_size: number
    io_pending: number
    cpu_pending: number
    io_active: number
    cpu_active: number
    max_configs: number
    scan_chunk: number
    thread_count: number
  }
  activity: {
    last_discovery_ts: number
    last_alive_confirmation_ts: number
    last_healthy_test_ts: number
    last_successful_download_ts: number
    last_scan_cycle_ts: number
  }
  censorship: {
    strategy: string
    network_type: string
    pressure_level: string
    cdn_reachable: boolean
    google_reachable: boolean
    telegram_reachable: boolean
    edge_router_bypass_active: boolean
    edge_router_bypass_status: string
  }
  runtime_config: {
    multiproxy_port: number
    gemini_port: number
    max_total: number
    max_workers: number
    scan_limit: number
    sleep_seconds: number
    xray_path: string
    singbox_path: string
    mihomo_path: string
    tor_path: string
    telegram_enabled: boolean
    telegram_limit: number
    telegram_timeout_ms: number
    targets_count: number
    github_urls_count: number
  }
  workers: WorkerInfo[]
  alive_configs: ConfigRecord[]
  telegram_only_configs: ConfigRecord[]
  history: number[]
  provisioned_ports: PortSlot[]
  balancers: BalancerInfo[]
  ws_stats?: {
    active_monitor_clients: number
    active_control_clients: number
    total_connections: number
    total_disconnections: number
    messages_sent: number
    messages_received: number
    pings_sent: number
  }
  generated_configs?: any[]
  generated_total?: number
  generated_active?: number
  generated_ts?: number
}

interface WorkerInfo {
  name: string
  state: string
  last_run: number
  last_error: string
  runs: number
  errors: number
  next_run_in: number
  extra: Record<string, string>
}

interface ConfigRecord {
  uri: string
  latency_ms: number
  engine_used: string
  first_seen: number
  last_alive: number
  last_tested: number
  total_tests: number
  total_passes: number
  consecutive_fails: number
  alive: boolean
  tag: string
  telegram_only?: boolean
}

interface PortSlot {
  port: number
  http_port: number
  mixed: boolean
  uri: string
  engine_used: string
  alive: boolean
  tcp_alive: boolean
  socks_ready: boolean
  http_ready: boolean
  latency_ms: number
  last_probe_ts: number
  consecutive_failures: number
}

interface BalancerInfo {
  port: number
  type: string
  running: boolean
  backends: number
  healthy: number
  tcp_alive: boolean
  socks_ready: boolean
  http_ready: boolean
  last_probe_ts: number
}

interface LogLine {
  ts: number
  text: string
}

function formatUptime(seconds: number): string {
  if (!seconds || seconds < 0) return '0m'
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  const s = Math.floor(seconds % 60)
  if (h > 0) return `${h}h ${m}m`
  if (m > 0) return `${m}m ${s}s`
  return `${s}s`
}

function formatTs(ts: number): string {
  if (!ts || ts <= 0) return '-'
  return new Date(ts * 1000).toLocaleTimeString()
}

function formatLatency(ms: number): string {
  if (ms <= 0) return '-'
  if (ms < 1000) return `${Math.round(ms)}ms`
  return `${(ms / 1000).toFixed(1)}s`
}

const stateColors: Record<string, 'success' | 'warning' | 'error' | 'default' | 'info'> = {
  running: 'success',
  idle: 'default',
  sleeping: 'warning',
  error: 'error',
  stopped: 'error',
}

export default function Home() {
  const [status, setStatus] = useState<HunterStatus | null>(null)
  const [connected, setConnected] = useState(false)
  const [wsState, setWsState] = useState<'connecting' | 'connected' | 'disconnected' | 'reconnecting'>('connecting')
  const [reconnectCount, setReconnectCount] = useState(0)
  const [wsStats, setWsStats] = useState<any>(null)
  const [logs, setLogs] = useState<LogLine[]>([])
  const [activeTab, setActiveTab] = useState(0)
  const [isClient, setIsClient] = useState(false)
  const [hostname, setHostname] = useState('localhost')
  const [snackbar, setSnackbar] = useState<{ open: boolean; msg: string; severity: 'success' | 'error' | 'info' | 'warning' }>({ open: false, msg: '', severity: 'info' })
  const [logVisible, setLogVisible] = useState(true)
  const [configFilter, setConfigFilter] = useState('')
  const [portPage, setPortPage] = useState(0)
  const [portRowsPerPage, setPortRowsPerPage] = useState(10)
  const [configPage, setConfigPage] = useState(0)
  const [configRowsPerPage, setConfigRowsPerPage] = useState(25)
  const [speedProfile, setSpeedProfile] = useState('medium')
  const [addConfigText, setAddConfigText] = useState('')
  const [showAddDialog, setShowAddDialog] = useState(false)
  const [clearHours, setClearHours] = useState(168)
  const [qrDialogOpen, setQrDialogOpen] = useState(false)
  const [qrUri, setQrUri] = useState('')
  const [qrDataUrl, setQrDataUrl] = useState('')
  const [detailDialogOpen, setDetailDialogOpen] = useState(false)
  const [detailConfig, setDetailConfig] = useState<ConfigRecord | null>(null)
  const [copiedUri, setCopiedUri] = useState<string | null>(null)
  const [gatewayMetrics, setGatewayMetrics] = useState<any>(null)
  const [upstreamTunnels, setUpstreamTunnels] = useState<any[]>([])
  const [generatedConfigs, setGeneratedConfigs] = useState<any[]>([])
  const [upstreamMetrics, setUpstreamMetrics] = useState<any>(null)
  const wsRef = useRef<WebSocket | null>(null)
  const reconnectTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const heartbeatTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const reconnectCountRef = useRef(0)
  const logContainerRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    setIsClient(true)
    setHostname(window.location.hostname)
  }, [])

  useEffect(() => {
    if (!isClient) return

    let mounted = true
    let ws: WebSocket | null = null
    let manualClose = false

    const backendHost = process.env.NEXT_PUBLIC_BACKEND_HOST || hostname
    const wsStatusPort = process.env.NEXT_PUBLIC_WS_STATUS_PORT || '7802'

    const proxyPath = process.env.NEXT_PUBLIC_PROXY_PATH || ''
    let wsUrl: string
    if (proxyPath) {
      const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
      wsUrl = `${proto}//${window.location.host}${proxyPath}/ws-status`
    } else {
      wsUrl = `ws://${backendHost}:${wsStatusPort}`
    }

    const connect = () => {
      if (!mounted) return
      setWsState(reconnectCountRef.current > 0 ? 'reconnecting' : 'connecting')
      ws = new WebSocket(wsUrl)
      wsRef.current = ws

      ws.onopen = () => {
        if (!mounted) return
        setConnected(true)
        setWsState('connected')
        if (reconnectCountRef.current > 0) {
          setSnackbar({ open: true, msg: `Reconnected to Hunter backend (attempt #${reconnectCountRef.current})`, severity: 'success' })
        } else {
          setSnackbar({ open: true, msg: 'Connected to Hunter backend', severity: 'success' })
        }

        // Client heartbeat - send ping every 30s via WebSocket API
        if (heartbeatTimerRef.current) clearTimeout(heartbeatTimerRef.current)
        heartbeatTimerRef.current = setInterval(() => {
          if (ws && ws.readyState === WebSocket.OPEN) {
            // Browser WebSocket API doesn't expose ping frames directly,
            // but we can send a custom heartbeat message that the server can use to track liveness
            ws.send(JSON.stringify({ type: 'heartbeat', ts: Date.now() }))
          }
        }, 30000)
      }

      ws.onmessage = (event) => {
        if (!mounted) return
        try {
          const data = JSON.parse(event.data)
          if (data.type === 'status') {
            setStatus(data.payload)
            if (data.payload?.speed?.profile) {
              setSpeedProfile(data.payload.speed.profile)
            }
            if (data.payload?.upstream_tunnels) {
              setUpstreamTunnels(data.payload.upstream_tunnels)
            }
            if (data.payload?.generated_configs) {
              setGeneratedConfigs(data.payload.generated_configs)
            }
            if (data.payload?.ws_stats) {
              setWsStats(data.payload.ws_stats)
            }
          } else if (data.type === 'logs') {
            if (data.payload?.lines && Array.isArray(data.payload.lines)) {
              const newLogs: LogLine[] = data.payload.lines.slice(-200).map((line: string) => ({
                ts: Date.now() / 1000,
                text: line,
              }))
              setLogs(newLogs)
            }
          } else if (data.type === 'log_append') {
            if (data.payload?.lines && Array.isArray(data.payload.lines)) {
              const appended: LogLine[] = (data.payload.lines as string[]).map((line: string) => ({
                ts: Date.now() / 1000,
                text: line,
              }))
              setLogs(prev => [...prev, ...appended].slice(-500))
            }
          } else if (data.type === 'discovery_log') {
            if (data.payload?.line) {
              setLogs(prev => [...prev.slice(-200), { ts: data.payload.ts || Date.now() / 1000, text: data.payload.line }])
            }
          } else if (data.type === 'gateway_metrics') {
            if (data.payload) {
              setGatewayMetrics(data.payload)
            }
          } else if (data.type === 'upstream_states') {
            if (data.payload?.upstream_states) {
              setUpstreamTunnels(data.payload.upstream_states)
            }
          } else if (data.type === 'upstream_metrics') {
            if (data.payload) {
              setUpstreamMetrics(data.payload)
            }
          } else if (data.type === 'generated_config_states') {
            if (data.payload?.generated_config_states) {
              setGeneratedConfigs(data.payload.generated_config_states)
            }
          } else if (data.type === 'ws_stats') {
            if (data.payload) {
              setWsStats(data.payload)
            }
          }
        } catch (e) {
          console.error('Failed to parse WebSocket message:', e)
        }
      }

      ws.onerror = () => {
        if (mounted) {
          setConnected(false)
          setWsState('disconnected')
        }
      }

      ws.onclose = () => {
        if (heartbeatTimerRef.current) {
          clearInterval(heartbeatTimerRef.current)
          heartbeatTimerRef.current = null
        }
        if (mounted && !manualClose) {
          setConnected(false)
          setWsState('reconnecting')
          reconnectCountRef.current++
          setReconnectCount(reconnectCountRef.current)

          // Exponential backoff: 1s, 2s, 4s, 8s, max 30s
          const delay = Math.min(1000 * Math.pow(2, reconnectCountRef.current - 1), 30000)
          if (reconnectTimerRef.current) clearTimeout(reconnectTimerRef.current)
          reconnectTimerRef.current = setTimeout(() => {
            if (mounted) connect()
          }, delay)
        }
      }
    }

    connect()

    return () => {
      mounted = false
      manualClose = true
      if (reconnectTimerRef.current) clearTimeout(reconnectTimerRef.current)
      if (heartbeatTimerRef.current) clearInterval(heartbeatTimerRef.current)
      if (ws) ws.close()
    }
  }, [isClient, hostname])

  useEffect(() => {
    if (logContainerRef.current && logVisible) {
      logContainerRef.current.scrollTop = logContainerRef.current.scrollHeight
    }
  }, [logs, logVisible])

  const sendCommand = useCallback((command: string, extra?: Record<string, unknown>) => {
    if (!isClient) return
    const backendHost = process.env.NEXT_PUBLIC_BACKEND_HOST || hostname
    const wsCommandPort = process.env.NEXT_PUBLIC_WS_COMMAND_PORT || '7801'
    const proxyPath = process.env.NEXT_PUBLIC_PROXY_PATH || ''
    let cmdUrl: string
    if (proxyPath) {
      const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
      cmdUrl = `${proto}//${window.location.host}${proxyPath}/ws-command`
    } else {
      cmdUrl = `ws://${backendHost}:${wsCommandPort}`
    }
    const ws = new WebSocket(cmdUrl)
    ws.onopen = () => {
      const payload = { command, ...(extra || {}) }
      ws.send(JSON.stringify(payload))
    }
    ws.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data)
        if (data.type === 'command_result') {
          setSnackbar({
            open: true,
            msg: data.message || (data.ok ? 'Command executed' : 'Command failed'),
            severity: data.ok ? 'success' : 'error',
          })
        }
      } catch (e) {
        console.error('Command response parse error:', e)
      }
      ws.close()
    }
    ws.onerror = () => {
      setSnackbar({ open: true, msg: 'Failed to send command', severity: 'error' })
    }
  }, [isClient, hostname])

  const downloadConfigs = async (type: 'gold' | 'silver' | 'all' | 'alive') => {
    if (!isClient) return
    try {
      const backendHost = process.env.NEXT_PUBLIC_BACKEND_HOST || hostname
      const apiPort = process.env.NEXT_PUBLIC_API_PORT || '7801'
      const proxyPath = process.env.NEXT_PUBLIC_PROXY_PATH || ''
      const apiUrl = proxyPath
        ? `${proxyPath}/api/configs/${type}`
        : `http://${backendHost}:${apiPort}/api/configs/${type}`
      const response = await fetch(apiUrl)
      if (!response.ok) {
        const errData = await response.json().catch(() => null)
        const msg = errData?.error || `HTTP ${response.status}`
        setSnackbar({ open: true, msg: `Download failed: ${msg}`, severity: 'error' })
        return
      }
      const data = await response.text()
      const lines = data.split('\n').filter((l) => l.trim().length > 0)
      const count = response.headers.get('X-Config-Count') || String(lines.length)
      if (lines.length === 0) {
        setSnackbar({ open: true, msg: `No ${type} configs available`, severity: 'warning' })
        return
      }
      const blob = new Blob([data], { type: 'text/plain' })
      saveAs(blob, `hunter_${type}_configs.txt`)
      setSnackbar({ open: true, msg: `Downloaded ${count} ${type} configs`, severity: 'success' })
    } catch (error) {
      const msg = error instanceof Error ? error.message : 'Unknown error'
      setSnackbar({ open: true, msg: `Download failed: ${msg}`, severity: 'error' })
    }
  }

  const downloadStatus = () => {
    if (!status) return
    const blob = new Blob([JSON.stringify(status, null, 2)], { type: 'application/json' })
    saveAs(blob, `hunter_status_${Date.now()}.json`)
  }

  const showQrCode = async (uri: string) => {
    try {
      const dataUrl = await QRCode.toDataURL(uri, {
        width: 512,
        margin: 2,
        errorCorrectionLevel: 'M',
        color: { dark: '#000000', light: '#ffffff' },
      })
      setQrUri(uri)
      setQrDataUrl(dataUrl)
      setQrDialogOpen(true)
    } catch (error) {
      setSnackbar({ open: true, msg: 'Failed to generate QR code', severity: 'error' })
    }
  }

  const copyToClipboard = async (text: string, label: string) => {
    try {
      await navigator.clipboard.writeText(text)
      setCopiedUri(text)
      setSnackbar({ open: true, msg: `${label} copied to clipboard`, severity: 'success' })
      setTimeout(() => setCopiedUri(null), 2000)
    } catch {
      setSnackbar({ open: true, msg: `Failed to copy ${label}`, severity: 'error' })
    }
  }

  const downloadSingleConfig = (uri: string) => {
    const blob = new Blob([uri], { type: 'text/plain' })
    const name = uri.split('://')[0] || 'config'
    saveAs(blob, `config_${name}_${Date.now()}.txt`)
    setSnackbar({ open: true, msg: 'Config downloaded', severity: 'success' })
  }

  const showConfigDetails = (cfg: ConfigRecord) => {
    setDetailConfig(cfg)
    setDetailDialogOpen(true)
  }

  const getProtocol = (uri: string): string => {
    const match = uri.match(/^([a-z]+):\/\//i)
    return match ? match[1].toUpperCase() : 'Unknown'
  }

  const handleAddConfigs = () => {
    if (!addConfigText.trim()) return
    sendCommand('add_configs', { configs: addConfigText })
    setShowAddDialog(false)
    setAddConfigText('')
  }

  if (!isClient) {
    return (
      <Box sx={{ display: 'flex', justifyContent: 'center', alignItems: 'center', minHeight: '100vh' }}>
        <CircularProgress />
      </Box>
    )
  }

  const filteredPorts = status?.provisioned_ports || []
  const pagedPorts = filteredPorts.slice(portPage * portRowsPerPage, portPage * portRowsPerPage + portRowsPerPage)

  const filteredConfigs = (status?.alive_configs || []).filter(c =>
    !configFilter || c.uri.toLowerCase().includes(configFilter.toLowerCase()) || c.tag?.toLowerCase().includes(configFilter.toLowerCase())
  )
  const pagedConfigs = filteredConfigs.slice(configPage * configRowsPerPage, configPage * configRowsPerPage + configRowsPerPage)

  const tabLabels = ['Dashboard', 'Proxy Tunnels', 'Configs', 'Dedicated Upstreams', 'Generated Config Pool', 'Active Proxies', 'Routing', 'Engine Status', 'NOC', 'Workers', 'Control', 'Settings']

  return (
    <Box sx={{ flexGrow: 1, bgcolor: 'background.default', minHeight: '100vh' }}>
      {/* AppBar */}
      <AppBar position="sticky" elevation={0} sx={{ borderBottom: 1, borderColor: 'divider' }}>
        <Toolbar>
          <Security sx={{ mr: 1.5, color: 'primary.main' }} />
          <Typography variant="h6" component="div" sx={{ fontWeight: 700 }}>
            Hunter
          </Typography>
          <Typography variant="body2" sx={{ ml: 1.5, color: 'text.secondary', display: { xs: 'none', sm: 'block' } }}>
            Proxy Configuration Discovery
          </Typography>
          <Box sx={{ flexGrow: 1 }} />
          <Stack direction="row" spacing={1.5} alignItems="center">
            <Chip
              icon={connected ? <Wifi /> : <WifiOff />}
              label={connected ? 'Connected' : wsState === 'reconnecting' ? `Reconnecting (${reconnectCount})` : wsState}
              color={connected ? 'success' : wsState === 'reconnecting' ? 'warning' : 'error'}
              size="small"
              variant="outlined"
            />
            <Chip
              label={status?.paused ? 'Paused' : status?.phase || 'Idle'}
              color={status?.paused ? 'warning' : status?.phase === 'running' ? 'success' : 'default'}
              size="small"
            />
            <Tooltip title={logVisible ? 'Hide Logs' : 'Show Logs'}>
              <IconButton size="small" onClick={() => setLogVisible(!logVisible)}>
                {logVisible ? <VisibilityOff /> : <Visibility />}
              </IconButton>
            </Tooltip>
          </Stack>
        </Toolbar>
      </AppBar>

      {/* Tabs */}
      <Box sx={{ borderBottom: 1, borderColor: 'divider', bgcolor: 'background.paper' }}>
        <Container maxWidth={false}>
          <Tabs value={activeTab} onChange={(_, v) => setActiveTab(v)} variant="scrollable" scrollButtons="auto">
            {tabLabels.map((label) => (
              <Tab key={label} label={label} />
            ))}
          </Tabs>
        </Container>
      </Box>

      <Container maxWidth={false} sx={{ mt: 3, mb: 10 }}>
        {/* === DASHBOARD TAB === */}
        {activeTab === 0 && (
          <Box>
            <Grid container spacing={2}>
              {/* Status Cards */}
              <Grid item xs={12} sm={6} md={3}>
                <Card>
                  <CardContent>
                    <Stack direction="row" justifyContent="space-between" alignItems="center">
                      <Box>
                        <Typography color="text.secondary" variant="body2">Phase</Typography>
                        <Typography variant="h5" sx={{ mt: 0.5 }}>
                          {status?.paused ? 'Paused' : status?.phase || 'Idle'}
                        </Typography>
                      </Box>
                      <Avatar sx={{ bgcolor: status?.phase === 'running' ? 'success.main' : 'default' }}>
                        {status?.paused ? <Pause /> : status?.phase === 'running' ? <PlayArrow /> : <Stop />}
                      </Avatar>
                    </Stack>
                  </CardContent>
                </Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card>
                  <CardContent>
                    <Stack direction="row" justifyContent="space-between" alignItems="center">
                      <Box>
                        <Typography color="text.secondary" variant="body2">Uptime</Typography>
                        <Typography variant="h5" sx={{ mt: 0.5 }}>{formatUptime(status?.uptime_s || 0)}</Typography>
                      </Box>
                      <Avatar sx={{ bgcolor: 'info.main' }}><TrendingUp /></Avatar>
                    </Stack>
                  </CardContent>
                </Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card>
                  <CardContent>
                    <Stack direction="row" justifyContent="space-between" alignItems="center">
                      <Box>
                        <Typography color="text.secondary" variant="body2">Alive Configs</Typography>
                        <Typography variant="h5" sx={{ mt: 0.5, color: 'success.main' }}>{status?.db?.alive || 0}</Typography>
                      </Box>
                      <Avatar sx={{ bgcolor: 'success.main' }}><CheckCircle /></Avatar>
                    </Stack>
                  </CardContent>
                </Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card>
                  <CardContent>
                    <Stack direction="row" justifyContent="space-between" alignItems="center">
                      <Box>
                        <Typography color="text.secondary" variant="body2">Total Configs</Typography>
                        <Typography variant="h5" sx={{ mt: 0.5 }}>{status?.db?.total || 0}</Typography>
                      </Box>
                      <Avatar sx={{ bgcolor: 'primary.main' }}><Dns /></Avatar>
                    </Stack>
                  </CardContent>
                </Card>
              </Grid>

              {/* DB Stats */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>Database Stats</Typography>
                    <Grid container spacing={2}>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Tested</Typography>
                        <Typography variant="h6">{status?.db?.tested_unique || 0}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Untested</Typography>
                        <Typography variant="h6">{status?.db?.untested_unique || 0}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Stale</Typography>
                        <Typography variant="h6" color="warning.main">{status?.db?.stale_unique || 0}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Avg Latency</Typography>
                        <Typography variant="h6">{formatLatency(status?.db?.avg_latency_ms || 0)}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Total Tests</Typography>
                        <Typography variant="h6">{status?.db?.total_tests || 0}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Total Passes</Typography>
                        <Typography variant="h6" color="success.main">{status?.db?.total_passes || 0}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Pending</Typography>
                        <Typography variant="h6" color="warning.main">{status?.pending_unique || 0}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">ETA</Typography>
                        <Typography variant="h6">{status?.eta_seconds ? formatUptime(status.eta_seconds) : '-'}</Typography>
                      </Grid>
                    </Grid>
                  </CardContent>
                </Card>
              </Grid>

              {/* Validator Stats */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>Validator</Typography>
                    <Grid container spacing={2}>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Last Tested</Typography>
                        <Typography variant="h6">{status?.validator?.last_tested || 0}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Last Passed</Typography>
                        <Typography variant="h6" color="success.main">{status?.validator?.last_passed || 0}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Rate/s</Typography>
                        <Typography variant="h6">{(status?.validator?.rate_per_s || 0).toFixed(1)}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Interval</Typography>
                        <Typography variant="h6">{status?.validator?.interval_s || 0}s</Typography>
                      </Grid>
                      <Grid item xs={12} sm={6}>
                        <Typography variant="body2" color="text.secondary">Active Processes</Typography>
                        <Box sx={{ display: 'flex', alignItems: 'center', gap: 1 }}>
                          <LinearProgress
                            variant="determinate"
                            value={((status?.validator?.active_test_processes || 0) / Math.max(status?.validator?.max_test_processes || 1, 1)) * 100}
                            sx={{ flexGrow: 1, mt: 0.5 }}
                          />
                          <Typography variant="body2">
                            {status?.validator?.active_test_processes || 0}/{status?.validator?.max_test_processes || 0}
                          </Typography>
                        </Box>
                      </Grid>
                      <Grid item xs={12} sm={6}>
                        <Typography variant="body2" color="text.secondary">Speed Profile</Typography>
                        <Chip label={status?.speed?.profile || 'unknown'} size="small" color="primary" sx={{ mt: 0.5 }} />
                      </Grid>
                    </Grid>
                  </CardContent>
                </Card>
              </Grid>

              {/* Hardware */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>
                      <Memory sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Hardware
                    </Typography>
                    <Grid container spacing={2}>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">CPU Usage</Typography>
                        <Box sx={{ display: 'flex', alignItems: 'center', gap: 1 }}>
                          <LinearProgress
                            variant="determinate"
                            value={status?.hardware?.cpu_percent || 0}
                            color={(status?.hardware?.cpu_percent || 0) > 80 ? 'error' : (status?.hardware?.cpu_percent || 0) > 60 ? 'warning' : 'success' as 'error' | 'warning' | 'success'}
                            sx={{ flexGrow: 1, mt: 0.5 }}
                          />
                          <Typography variant="body2">{Math.round(status?.hardware?.cpu_percent || 0)}%</Typography>
                        </Box>
                        <Typography variant="caption" color="text.secondary">
                          {status?.hardware?.cpu_count || 0} cores, {status?.hardware?.thread_count || 0} threads
                        </Typography>
                      </Grid>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">RAM Usage</Typography>
                        <Box sx={{ display: 'flex', alignItems: 'center', gap: 1 }}>
                          <LinearProgress
                            variant="determinate"
                            value={status?.hardware?.ram_percent || 0}
                            color={(status?.hardware?.ram_percent || 0) > 80 ? 'error' : (status?.hardware?.ram_percent || 0) > 60 ? 'warning' : 'success' as 'error' | 'warning' | 'success'}
                            sx={{ flexGrow: 1, mt: 0.5 }}
                          />
                          <Typography variant="body2">{Math.round(status?.hardware?.ram_percent || 0)}%</Typography>
                        </Box>
                        <Typography variant="caption" color="text.secondary">
                          {(status?.hardware?.ram_used_gb || 0).toFixed(1)} / {(status?.hardware?.ram_total_gb || 0).toFixed(1)} GB
                        </Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">IO Pool</Typography>
                        <Typography variant="body1">{status?.hardware?.io_active || 0}/{status?.hardware?.io_pool_size || 0}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">CPU Pool</Typography>
                        <Typography variant="body1">{status?.hardware?.cpu_active || 0}/{status?.hardware?.cpu_pool_size || 0}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Mode</Typography>
                        <Chip label={status?.hardware?.mode || 'unknown'} size="small" />
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Max Configs</Typography>
                        <Typography variant="body1">{status?.hardware?.max_configs || 0}</Typography>
                      </Grid>
                    </Grid>
                  </CardContent>
                </Card>
              </Grid>

              {/* Censorship */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>
                      <Science sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Censorship Detection
                    </Typography>
                    <Grid container spacing={2}>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Network</Typography>
                        <Chip
                          label={status?.censorship?.network_type || 'unknown'}
                          size="small"
                          color={status?.censorship?.network_type === 'censored' ? 'error' : 'success'}
                        />
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Pressure</Typography>
                        <Chip label={status?.censorship?.pressure_level || 'normal'} size="small" color="warning" />
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Strategy</Typography>
                        <Typography variant="body1">{status?.censorship?.strategy || 'none'}</Typography>
                      </Grid>
                      <Grid item xs={6} sm={3}>
                        <Typography variant="body2" color="text.secondary">Edge Bypass</Typography>
                        <Chip
                          label={status?.censorship?.edge_router_bypass_active ? 'Active' : 'Inactive'}
                          size="small"
                          color={status?.censorship?.edge_router_bypass_active ? 'success' : 'default'}
                        />
                      </Grid>
                      <Grid item xs={4}>
                        <Chip
                          icon={status?.censorship?.cdn_reachable ? <CheckCircle /> : <ErrorIcon />}
                          label="CDN"
                          size="small"
                          color={status?.censorship?.cdn_reachable ? 'success' : 'error'}
                          variant="outlined"
                        />
                      </Grid>
                      <Grid item xs={4}>
                        <Chip
                          icon={status?.censorship?.google_reachable ? <CheckCircle /> : <ErrorIcon />}
                          label="Google"
                          size="small"
                          color={status?.censorship?.google_reachable ? 'success' : 'error'}
                          variant="outlined"
                        />
                      </Grid>
                      <Grid item xs={4}>
                        <Chip
                          icon={status?.censorship?.telegram_reachable ? <CheckCircle /> : <ErrorIcon />}
                          label="Telegram"
                          size="small"
                          color={status?.censorship?.telegram_reachable ? 'success' : 'error'}
                          variant="outlined"
                        />
                      </Grid>
                    </Grid>
                  </CardContent>
                </Card>
              </Grid>

              {/* Balancers */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>
                      <Router sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Load Balancers
                    </Typography>
                    <TableContainer>
                      <Table size="small">
                        <TableHead>
                          <TableRow>
                            <TableCell>Type</TableCell>
                            <TableCell>Port</TableCell>
                            <TableCell>Status</TableCell>
                            <TableCell>Backends</TableCell>
                            <TableCell>Healthy</TableCell>
                            <TableCell>SOCKS</TableCell>
                            <TableCell>HTTP</TableCell>
                          </TableRow>
                        </TableHead>
                        <TableBody>
                          {(status?.balancers || []).map((bal) => (
                            <TableRow key={bal.type}>
                              <TableCell sx={{ textTransform: 'capitalize' }}>{bal.type}</TableCell>
                              <TableCell sx={{ fontFamily: 'monospace' }}>{hostname}:{bal.port}</TableCell>
                              <TableCell>
                                <Chip label={bal.running ? 'Running' : 'Stopped'} size="small" color={bal.running ? 'success' : 'error'} />
                              </TableCell>
                              <TableCell>{bal.backends}</TableCell>
                              <TableCell sx={{ color: 'success.main' }}>{bal.healthy}</TableCell>
                              <TableCell>{bal.socks_ready ? '✓' : '✗'}</TableCell>
                              <TableCell>{bal.http_ready ? '✓' : '✗'}</TableCell>
                            </TableRow>
                          ))}
                        </TableBody>
                      </Table>
                    </TableContainer>
                  </CardContent>
                </Card>
              </Grid>

              {/* Activity */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>
                      <Bolt sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Recent Activity
                    </Typography>
                    <List dense>
                      <ListItem>
                        <ListItemText
                          primary="Last Discovery"
                          secondary={formatTs(status?.activity?.last_discovery_ts || 0)}
                        />
                      </ListItem>
                      <ListItem>
                        <ListItemText
                          primary="Last Alive Confirmation"
                          secondary={formatTs(status?.activity?.last_alive_confirmation_ts || 0)}
                        />
                      </ListItem>
                      <ListItem>
                        <ListItemText
                          primary="Last Health Test"
                          secondary={formatTs(status?.activity?.last_healthy_test_ts || 0)}
                        />
                      </ListItem>
                      <ListItem>
                        <ListItemText
                          primary="Last Download"
                          secondary={formatTs(status?.activity?.last_successful_download_ts || 0)}
                        />
                      </ListItem>
                      <ListItem>
                        <ListItemText
                          primary="Last Scan Cycle"
                          secondary={formatTs(status?.activity?.last_scan_cycle_ts || 0)}
                        />
                      </ListItem>
                    </List>
                  </CardContent>
                </Card>
              </Grid>

              {/* Dedicated Upstreams Summary */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>
                      <Cloud sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Dedicated Upstreams
                    </Typography>
                    <Grid container spacing={2}>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Connected</Typography>
                        <Typography variant="h5" color="success.main">
                          {upstreamTunnels.filter((u: any) => u.connected).length} / {upstreamTunnels.length}
                        </Typography>
                      </Grid>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Total Reconnects</Typography>
                        <Typography variant="h5">{upstreamTunnels.reduce((s: number, u: any) => s + (u.reconnect_count || 0), 0)}</Typography>
                      </Grid>
                    </Grid>
                    {upstreamTunnels.length > 0 && (
                      <Box sx={{ mt: 1 }}>
                        {upstreamTunnels.map((u: any, i: number) => (
                          <Chip
                            key={i}
                            label={`${u.name}: ${u.connected ? 'OK' : 'DOWN'}${u.latency_ms ? ' ' + u.latency_ms.toFixed(0) + 'ms' : ''}`}
                            color={u.connected ? 'success' : 'error'}
                            size="small"
                            sx={{ mr: 0.5, mb: 0.5 }}
                          />
                        ))}
                      </Box>
                    )}
                  </CardContent>
                </Card>
              </Grid>

              {/* Generated Configs Summary */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>
                      <Widgets sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Generated Config Pool
                    </Typography>
                    <Grid container spacing={2}>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Active Configs</Typography>
                        <Typography variant="h5" color="success.main">
                          {generatedConfigs.filter((c: any) => c.active).length} / {generatedConfigs.length}
                        </Typography>
                      </Grid>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Pool Range</Typography>
                        <Typography variant="h5" sx={{ fontFamily: 'monospace', fontSize: '1rem' }}>
                          abhar-01..{generatedConfigs.length > 0 ? `abhar-${String(generatedConfigs.length).padStart(2, '0')}` : '20'}
                        </Typography>
                      </Grid>
                    </Grid>
                    <Box sx={{ mt: 1, display: 'flex', gap: 1, flexWrap: 'wrap' }}>
                      <Button
                        size="small"
                        variant="outlined"
                        href={(process.env.NEXT_PUBLIC_PROXY_PATH || '') + '/api/myconfigs/generated'}
                        target="_blank"
                      >
                        Download
                      </Button>
                      <Button
                        size="small"
                        variant="outlined"
                        href={(process.env.NEXT_PUBLIC_PROXY_PATH || '') + '/api/myconfigs/texts'}
                        target="_blank"
                      >
                        JSON
                      </Button>
                    </Box>
                  </CardContent>
                </Card>
              </Grid>
            </Grid>

            {/* Live Logs */}
            {logVisible && (
              <Card sx={{ mt: 2 }}>
                <CardContent>
                  <Stack direction="row" justifyContent="space-between" alignItems="center" sx={{ mb: 1 }}>
                    <Typography variant="h6">
                      <Terminal sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Live Logs
                    </Typography>
                    <Button size="small" startIcon={<Delete />} onClick={() => setLogs([])}>Clear</Button>
                  </Stack>
                  <Box
                    ref={logContainerRef}
                    sx={{
                      height: 250,
                      overflow: 'auto',
                      bgcolor: '#0a0f1e',
                      borderRadius: 1,
                      p: 1,
                      fontFamily: 'monospace',
                      fontSize: '0.75rem',
                      lineHeight: 1.4,
                    }}
                  >
                    {logs.length === 0 ? (
                      <Typography color="text.secondary" sx={{ p: 2 }}>Waiting for logs...</Typography>
                    ) : (
                      logs.map((log, i) => (
                        <Box key={i} sx={{ mb: 0.25, color: log.text.includes('[ERROR]') || log.text.includes('error') ? 'error.main' : log.text.includes('[WARN]') || log.text.includes('warn') ? 'warning.main' : 'text.primary' }}>
                          <span style={{ color: '#64748b' }}>[{formatTs(log.ts)}]</span> {log.text}
                        </Box>
                      ))
                    )}
                  </Box>
                </CardContent>
              </Card>
            )}
          </Box>
        )}

        {/* === PROXY TUNNELS TAB === */}
        {activeTab === 1 && (
          <Box>
            <Grid container spacing={2} sx={{ mb: 2 }}>
              <Grid item xs={12} sm={6} md={3}>
                <Card>
                  <CardContent>
                    <Typography color="text.secondary" variant="body2">Total Tunnels</Typography>
                    <Typography variant="h4">{status?.provisioned_ports?.length || 0}</Typography>
                  </CardContent>
                </Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card>
                  <CardContent>
                    <Typography color="text.secondary" variant="body2">Alive</Typography>
                    <Typography variant="h4" color="success.main">
                      {status?.provisioned_ports?.filter(p => p.alive).length || 0}
                    </Typography>
                  </CardContent>
                </Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card>
                  <CardContent>
                    <Typography color="text.secondary" variant="body2">SOCKS Ready</Typography>
                    <Typography variant="h4" color="info.main">
                      {status?.provisioned_ports?.filter(p => p.socks_ready).length || 0}
                    </Typography>
                  </CardContent>
                </Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card>
                  <CardContent>
                    <Typography color="text.secondary" variant="body2">HTTP Ready</Typography>
                    <Typography variant="h4" color="secondary.main">
                      {status?.provisioned_ports?.filter(p => p.http_ready).length || 0}
                    </Typography>
                  </CardContent>
                </Card>
              </Grid>
            </Grid>

            <Stack direction="row" spacing={1} sx={{ mb: 2 }}>
              <Button variant="contained" startIcon={<Refresh />} onClick={() => sendCommand('refresh_ports')}>
                Refresh Ports
              </Button>
              <Button variant="outlined" startIcon={<Speed />} onClick={() => sendCommand('recheck_live_ports')}>
                Recheck Live
              </Button>
              <Button variant="outlined" color="warning" startIcon={<Build />} onClick={() => sendCommand('reprovision_ports')}>
                Reprovision All
              </Button>
            </Stack>

            <Card>
              <CardContent>
                <Typography variant="h6" gutterBottom>Provisioned Proxy Tunnels</Typography>
                <TableContainer component={Paper}>
                  <Table size="small">
                    <TableHead>
                      <TableRow>
                        <TableCell>Port</TableCell>
                        <TableCell>Engine</TableCell>
                        <TableCell>URI</TableCell>
                        <TableCell>Alive</TableCell>
                        <TableCell>TCP</TableCell>
                        <TableCell>SOCKS</TableCell>
                        <TableCell>HTTP</TableCell>
                        <TableCell>Latency</TableCell>
                        <TableCell>Fails</TableCell>
                        <TableCell>Last Probe</TableCell>
                      </TableRow>
                    </TableHead>
                    <TableBody>
                      {pagedPorts.map((port) => (
                        <TableRow key={port.port} hover>
                          <TableCell sx={{ fontFamily: 'monospace' }}>{hostname}:{port.port}</TableCell>
                          <TableCell><Chip label={port.engine_used} size="small" /></TableCell>
                          <TableCell sx={{ maxWidth: 200, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                            {port.uri}
                          </TableCell>
                          <TableCell>{port.alive ? <CheckCircle color="success" fontSize="small" /> : <ErrorIcon color="error" fontSize="small" />}</TableCell>
                          <TableCell>{port.tcp_alive ? '✓' : '✗'}</TableCell>
                          <TableCell>{port.socks_ready ? '✓' : '✗'}</TableCell>
                          <TableCell>{port.http_ready ? '✓' : '✗'}</TableCell>
                          <TableCell>{formatLatency(port.latency_ms)}</TableCell>
                          <TableCell>
                            <Chip label={port.consecutive_failures} size="small" color={port.consecutive_failures > 3 ? 'error' : 'default'} />
                          </TableCell>
                          <TableCell>{formatTs(port.last_probe_ts)}</TableCell>
                        </TableRow>
                      ))}
                      {pagedPorts.length === 0 && (
                        <TableRow>
                          <TableCell colSpan={10} align="center">
                            <Typography color="text.secondary">No provisioned ports</Typography>
                          </TableCell>
                        </TableRow>
                      )}
                    </TableBody>
                  </Table>
                </TableContainer>
                <TablePagination
                  component="div"
                  count={status?.provisioned_ports?.length || 0}
                  page={portPage}
                  onPageChange={(_, p) => setPortPage(p)}
                  rowsPerPage={portRowsPerPage}
                  onRowsPerPageChange={(e) => { setPortRowsPerPage(parseInt(e.target.value, 10)); setPortPage(0) }}
                />
              </CardContent>
            </Card>
          </Box>
        )}

        {/* === CONFIGS TAB === */}
        {activeTab === 2 && (
          <Box>
            <Grid container spacing={2} sx={{ mb: 2 }}>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Alive</Typography>
                  <Typography variant="h4" color="success.main">{status?.db?.alive || 0}</Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Telegram Only</Typography>
                  <Typography variant="h4" color="info.main">{status?.telegram_only_configs?.length || 0}</Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Avg Latency</Typography>
                  <Typography variant="h4">{formatLatency(status?.db?.avg_latency_ms || 0)}</Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Pass Rate</Typography>
                  <Typography variant="h4">
                    {status?.db?.total_tests ? ((status.db.total_passes / status.db.total_tests) * 100).toFixed(1) : 0}%
                  </Typography>
                </CardContent></Card>
              </Grid>
            </Grid>

            <Stack direction="row" spacing={1} sx={{ mb: 2 }} flexWrap="wrap">
              <Button variant="contained" color="success" startIcon={<Download />} onClick={() => downloadConfigs('gold')}>
                Download Gold
              </Button>
              <Button variant="contained" color="warning" startIcon={<Download />} onClick={() => downloadConfigs('silver')}>
                Download Silver
              </Button>
              <Button variant="contained" startIcon={<Download />} onClick={() => downloadConfigs('all')}>
                Download All
              </Button>
              <Button variant="outlined" startIcon={<Add />} onClick={() => setShowAddDialog(true)}>
                Add Configs
              </Button>
              <Button variant="outlined" startIcon={<CloudUpload />} onClick={() => sendCommand('load_raw_files')}>
                Load Raw Files
              </Button>
              <Button variant="outlined" startIcon={<CloudUpload />} onClick={() => sendCommand('load_bundle_files')}>
                Load Bundles
              </Button>
              <Button variant="outlined" startIcon={<Download />} onClick={downloadStatus}>
                Export Status
              </Button>
            </Stack>

            <Card>
              <CardContent>
                <Stack direction="row" justifyContent="space-between" alignItems="center" sx={{ mb: 2 }}>
                  <Typography variant="h6">Alive Configurations</Typography>
                  <TextField
                    size="small"
                    placeholder="Filter by URI or tag..."
                    value={configFilter}
                    onChange={(e) => { setConfigFilter(e.target.value); setConfigPage(0) }}
                    InputProps={{ startAdornment: <Search sx={{ mr: 1, color: 'text.secondary' }} /> }}
                  />
                </Stack>
                <TableContainer component={Paper}>
                  <Table size="small">
                    <TableHead>
                      <TableRow>
                        <TableCell>URI</TableCell>
                        <TableCell>Protocol</TableCell>
                        <TableCell>Engine</TableCell>
                        <TableCell>Latency</TableCell>
                        <TableCell>Tag</TableCell>
                        <TableCell>Tests</TableCell>
                        <TableCell>Passes</TableCell>
                        <TableCell>Fails</TableCell>
                        <TableCell>Last Tested</TableCell>
                        <TableCell align="center">Actions</TableCell>
                      </TableRow>
                    </TableHead>
                    <TableBody>
                      {pagedConfigs.map((cfg, i) => (
                        <TableRow key={i} hover>
                          <TableCell sx={{ maxWidth: 250, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', fontFamily: 'monospace', fontSize: '0.75rem' }}>
                            {cfg.uri}
                          </TableCell>
                          <TableCell><Chip label={getProtocol(cfg.uri)} size="small" color="primary" variant="outlined" /></TableCell>
                          <TableCell><Chip label={cfg.engine_used || '-'} size="small" /></TableCell>
                          <TableCell>{formatLatency(cfg.latency_ms)}</TableCell>
                          <TableCell><Chip label={cfg.tag || '-'} size="small" variant="outlined" /></TableCell>
                          <TableCell>{cfg.total_tests}</TableCell>
                          <TableCell sx={{ color: 'success.main' }}>{cfg.total_passes}</TableCell>
                          <TableCell sx={{ color: cfg.consecutive_fails > 3 ? 'error.main' : 'text.primary' }}>{cfg.consecutive_fails}</TableCell>
                          <TableCell>{formatTs(cfg.last_tested)}</TableCell>
                          <TableCell>
                            <Stack direction="row" spacing={0.5} justifyContent="center">
                              <Tooltip title="View Details">
                                <IconButton size="small" onClick={() => showConfigDetails(cfg)}>
                                  <Visibility fontSize="small" />
                                </IconButton>
                              </Tooltip>
                              <Tooltip title="Copy URI">
                                <IconButton size="small" onClick={() => copyToClipboard(cfg.uri, 'URI')}>
                                  <ContentCopy fontSize="small" color={copiedUri === cfg.uri ? 'success' : 'inherit'} />
                                </IconButton>
                              </Tooltip>
                              <Tooltip title="Show QR Code">
                                <IconButton size="small" onClick={() => showQrCode(cfg.uri)}>
                                  <QrCode2 fontSize="small" color="primary" />
                                </IconButton>
                              </Tooltip>
                              <Tooltip title="Download Single Config">
                                <IconButton size="small" onClick={() => downloadSingleConfig(cfg.uri)}>
                                  <Download fontSize="small" />
                                </IconButton>
                              </Tooltip>
                            </Stack>
                          </TableCell>
                        </TableRow>
                      ))}
                      {pagedConfigs.length === 0 && (
                        <TableRow>
                          <TableCell colSpan={10} align="center">
                            <Typography color="text.secondary">No configs found</Typography>
                          </TableCell>
                        </TableRow>
                      )}
                    </TableBody>
                  </Table>
                </TableContainer>
                <TablePagination
                  component="div"
                  count={filteredConfigs.length}
                  page={configPage}
                  onPageChange={(_, p) => setConfigPage(p)}
                  rowsPerPage={configRowsPerPage}
                  onRowsPerPageChange={(e) => { setConfigRowsPerPage(parseInt(e.target.value, 10)); setConfigPage(0) }}
                />
              </CardContent>
            </Card>
          </Box>
        )}

        {/* === DEDICATED UPSTREAMS TAB === */}
        {activeTab === 3 && (
          <Box sx={{ p: 3 }}>
            <Grid container spacing={3}>
              <Grid item xs={12}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>Dedicated Upstream Servers</Typography>
                    <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>
                      Permanent SSH SOCKS tunnel providers — automatically established, monitored, and reconnected.
                    </Typography>
                    <Grid container spacing={2} sx={{ mb: 2 }}>
                      <Grid item xs={12} sm={6} md={3}>
                        <Paper sx={{ p: 2, textAlign: 'center' }}>
                          <Typography variant="h4" color="primary.main">
                            {upstreamTunnels.filter((u: any) => u.connected).length}
                          </Typography>
                          <Typography variant="body2">Connected</Typography>
                        </Paper>
                      </Grid>
                      <Grid item xs={12} sm={6} md={3}>
                        <Paper sx={{ p: 2, textAlign: 'center' }}>
                          <Typography variant="h4" color="text.secondary">
                            {upstreamTunnels.length}
                          </Typography>
                          <Typography variant="body2">Total Upstreams</Typography>
                        </Paper>
                      </Grid>
                      <Grid item xs={12} sm={6} md={3}>
                        <Paper sx={{ p: 2, textAlign: 'center' }}>
                          <Typography variant="h4" color="success.main">
                            {upstreamTunnels.filter((u: any) => u.preferred).length}
                          </Typography>
                          <Typography variant="body2">Preferred</Typography>
                        </Paper>
                      </Grid>
                      <Grid item xs={12} sm={6} md={3}>
                        <Paper sx={{ p: 2, textAlign: 'center' }}>
                          <Typography variant="h4" color="warning.main">
                            {upstreamTunnels.reduce((sum: number, u: any) => sum + (u.reconnect_count || 0), 0)}
                          </Typography>
                          <Typography variant="body2">Total Reconnects</Typography>
                        </Paper>
                      </Grid>
                    </Grid>
                    <Table size="small">
                      <TableHead>
                        <TableRow>
                          <TableCell>Name</TableCell>
                          <TableCell>Host</TableCell>
                          <TableCell>SOCKS Port</TableCell>
                          <TableCell>Status</TableCell>
                          <TableCell>Preferred</TableCell>
                          <TableCell>Latency</TableCell>
                          <TableCell>Reconnects</TableCell>
                          <TableCell>Failures</TableCell>
                        </TableRow>
                      </TableHead>
                      <TableBody>
                        {upstreamTunnels.length === 0 ? (
                          <TableRow>
                            <TableCell colSpan={8} align="center">
                              <Typography variant="body2" color="text.secondary" sx={{ py: 2 }}>
                                No upstream tunnels configured. Waiting for backend...
                              </Typography>
                            </TableCell>
                          </TableRow>
                        ) : (
                          upstreamTunnels.map((u: any, i: number) => (
                            <TableRow key={i}>
                              <TableCell><strong>{u.name}</strong></TableCell>
                              <TableCell>{u.host}</TableCell>
                              <TableCell>{u.port}</TableCell>
                              <TableCell>
                                <Chip
                                  label={u.connected ? 'Connected' : 'Disconnected'}
                                  color={u.connected ? 'success' : 'error'}
                                  size="small"
                                />
                              </TableCell>
                              <TableCell>{u.preferred ? 'Yes' : 'No'}</TableCell>
                              <TableCell>{u.latency_ms ? `${u.latency_ms.toFixed(0)}ms` : '—'}</TableCell>
                              <TableCell>{u.reconnect_count || 0}</TableCell>
                              <TableCell>{u.consecutive_failures || 0}</TableCell>
                            </TableRow>
                          ))
                        )}
                      </TableBody>
                    </Table>
                  </CardContent>
                </Card>
              </Grid>
            </Grid>
          </Box>
        )}

        {/* === GENERATED CONFIG POOL TAB === */}
        {activeTab === 4 && (
          <Box sx={{ p: 3 }}>
            <Grid container spacing={3}>
              <Grid item xs={12}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>Generated Configuration Pool</Typography>
                    <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>
                      20 permanent public configurations (abhar-01 through abhar-20). These remain stable — backend routes may change, but public configs stay the same.
                    </Typography>
                    <Grid container spacing={2} sx={{ mb: 2 }}>
                      <Grid item xs={12} sm={6} md={3}>
                        <Paper sx={{ p: 2, textAlign: 'center' }}>
                          <Typography variant="h4" color="success.main">
                            {generatedConfigs.filter((c: any) => c.active).length}
                          </Typography>
                          <Typography variant="body2">Active</Typography>
                        </Paper>
                      </Grid>
                      <Grid item xs={12} sm={6} md={3}>
                        <Paper sx={{ p: 2, textAlign: 'center' }}>
                          <Typography variant="h4" color="primary.main">
                            {generatedConfigs.length}
                          </Typography>
                          <Typography variant="body2">Total</Typography>
                        </Paper>
                      </Grid>
                      <Grid item xs={12} sm={6} md={3}>
                        <Paper sx={{ p: 2, textAlign: 'center' }}>
                          <Typography variant="h4" color="text.secondary">
                            {generatedConfigs.filter((c: any) => c.upstream).map((c: any) => c.upstream).filter((v: string, idx: number, a: string[]) => a.indexOf(v) === idx).length}
                          </Typography>
                          <Typography variant="body2">Upstreams Used</Typography>
                        </Paper>
                      </Grid>
                      <Grid item xs={12} sm={6} md={3}>
                        <Paper sx={{ p: 2, textAlign: 'center' }}>
                          <Typography variant="h4" color="warning.main">
                            {generatedConfigs.filter((c: any) => !c.active).length}
                          </Typography>
                          <Typography variant="body2">Inactive</Typography>
                        </Paper>
                      </Grid>
                    </Grid>
                    <Box sx={{ mb: 2, display: 'flex', gap: 1, flexWrap: 'wrap' }}>
                      <Button
                        size="small"
                        variant="outlined"
                        onClick={() => {
                          const proxyPath = process.env.NEXT_PUBLIC_PROXY_PATH || ''
                          const base = proxyPath ? `${proxyPath}/api/myconfigs/generated` : '/api/myconfigs/generated'
                          window.open(base, '_blank')
                        }}
                      >
                        Download Generated
                      </Button>
                      <Button
                        size="small"
                        variant="outlined"
                        onClick={() => {
                          const proxyPath = process.env.NEXT_PUBLIC_PROXY_PATH || ''
                          const base = proxyPath ? `${proxyPath}/api/myconfigs/texts` : '/api/myconfigs/texts'
                          window.open(base, '_blank')
                        }}
                      >
                        View All as JSON
                      </Button>
                    </Box>
                    <Table size="small">
                      <TableHead>
                        <TableRow>
                          <TableCell>Name</TableCell>
                          <TableCell>Protocol</TableCell>
                          <TableCell>Host</TableCell>
                          <TableCell>Port</TableCell>
                          <TableCell>Upstream</TableCell>
                          <TableCell>Status</TableCell>
                          <TableCell>Latency</TableCell>
                          <TableCell>Actions</TableCell>
                        </TableRow>
                      </TableHead>
                      <TableBody>
                        {generatedConfigs.length === 0 ? (
                          <TableRow>
                            <TableCell colSpan={8} align="center">
                              <Typography variant="body2" color="text.secondary" sx={{ py: 2 }}>
                                No generated configs. Waiting for backend...
                              </Typography>
                            </TableCell>
                          </TableRow>
                        ) : (
                          generatedConfigs.map((c: any, i: number) => {
                            const uri = `socks5://${c.host || 'api.abharcable.com'}:${c.port}#${c.name}`
                            return (
                              <TableRow key={i}>
                                <TableCell><strong>{c.name}</strong></TableCell>
                                <TableCell>socks5</TableCell>
                                <TableCell>{c.host || 'api.abharcable.com'}</TableCell>
                                <TableCell>{c.port}</TableCell>
                                <TableCell>{c.upstream || '—'}</TableCell>
                                <TableCell>
                                  <Chip
                                    label={c.active ? 'Active' : 'Inactive'}
                                    color={c.active ? 'success' : 'default'}
                                    size="small"
                                  />
                                </TableCell>
                                <TableCell>{c.latency_ms ? `${c.latency_ms.toFixed(0)}ms` : '—'}</TableCell>
                                <TableCell>
                                  <Stack direction="row" spacing={0.5}>
                                    <Tooltip title="Copy URI">
                                      <IconButton size="small" onClick={() => {
                                        navigator.clipboard.writeText(uri)
                                        setCopiedUri(c.name)
                                        setTimeout(() => setCopiedUri(null), 2000)
                                      }}>
                                        <ContentCopy fontSize="small" />
                                      </IconButton>
                                    </Tooltip>
                                    <Tooltip title="QR Code">
                                      <IconButton size="small" onClick={() => showQrCode(uri)}>
                                        <QrCode2 fontSize="small" />
                                      </IconButton>
                                    </Tooltip>
                                    <Tooltip title="Download Config">
                                      <IconButton size="small" onClick={() => downloadSingleConfig(uri)}>
                                        <Download fontSize="small" />
                                      </IconButton>
                                    </Tooltip>
                                  </Stack>
                                </TableCell>
                              </TableRow>
                            )
                          })
                        )}
                      </TableBody>
                    </Table>
                  </CardContent>
                </Card>
              </Grid>
            </Grid>
          </Box>
        )}

        {/* === ACTIVE PROXIES TAB === */}
        {activeTab === 5 && (
          <Box>
            <Grid container spacing={2} sx={{ mb: 2 }}>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Active Instances</Typography>
                  <Typography variant="h4" color="primary.main">
                    {status?.provisioned_ports?.filter(p => p.alive).length || 0}
                  </Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Total Provisioned</Typography>
                  <Typography variant="h4">{status?.provisioned_ports?.length || 0}</Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Balancer Backends</Typography>
                  <Typography variant="h4" color="success.main">{status?.balancer_backends || 0}</Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Avg Latency</Typography>
                  <Typography variant="h4">{formatLatency(status?.db?.avg_latency_ms || 0)}</Typography>
                </CardContent></Card>
              </Grid>
            </Grid>

            <Card>
              <CardContent>
                <Typography variant="h6" gutterBottom>
                  <Hub sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                  Active Proxy Instances
                </Typography>
                <TableContainer component={Paper}>
                  <Table size="small">
                    <TableHead>
                      <TableRow>
                        <TableCell>Port</TableCell>
                        <TableCell>Protocol</TableCell>
                        <TableCell>Engine</TableCell>
                        <TableCell>URI</TableCell>
                        <TableCell>Latency</TableCell>
                        <TableCell>Health</TableCell>
                        <TableCell>TCP</TableCell>
                        <TableCell>SOCKS</TableCell>
                        <TableCell>HTTP</TableCell>
                        <TableCell>Failures</TableCell>
                        <TableCell>Last Probe</TableCell>
                      </TableRow>
                    </TableHead>
                    <TableBody>
                      {(status?.provisioned_ports || []).map((port, i) => (
                        <TableRow key={i} hover>
                          <TableCell sx={{ fontFamily: 'monospace' }}>{port.port}</TableCell>
                          <TableCell><Chip label={getProtocol(port.uri)} size="small" color="primary" variant="outlined" /></TableCell>
                          <TableCell><Chip label={port.engine_used || '-'} size="small" /></TableCell>
                          <TableCell sx={{ maxWidth: 200, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', fontFamily: 'monospace', fontSize: '0.75rem' }}>
                            {port.uri}
                          </TableCell>
                          <TableCell>{formatLatency(port.latency_ms)}</TableCell>
                          <TableCell>
                            <Chip
                              label={port.alive ? 'HEALTHY' : 'DEAD'}
                              size="small"
                              color={port.alive ? 'success' : 'error'}
                            />
                          </TableCell>
                          <TableCell>{port.tcp_alive ? '✓' : '✗'}</TableCell>
                          <TableCell>{port.socks_ready ? '✓' : '✗'}</TableCell>
                          <TableCell>{port.http_ready ? '✓' : '✗'}</TableCell>
                          <TableCell sx={{ color: port.consecutive_failures > 3 ? 'error.main' : 'text.primary' }}>
                            {port.consecutive_failures}
                          </TableCell>
                          <TableCell>{formatTs(port.last_probe_ts)}</TableCell>
                        </TableRow>
                      ))}
                      {(status?.provisioned_ports || []).length === 0 && (
                        <TableRow>
                          <TableCell colSpan={11} align="center">
                            <Typography color="text.secondary">No provisioned proxy ports</Typography>
                          </TableCell>
                        </TableRow>
                      )}
                    </TableBody>
                  </Table>
                </TableContainer>
              </CardContent>
            </Card>
          </Box>
        )}

        {/* === ROUTING TAB === */}
        {activeTab === 6 && (
          <Box>
            <Grid container spacing={2} sx={{ mb: 2 }}>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Gateway Port</Typography>
                  <Typography variant="h4" sx={{ fontFamily: 'monospace' }}>7805</Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Active Sessions</Typography>
                  <Typography variant="h4" color="primary.main">
                    {gatewayMetrics?.active_sessions || 0}
                  </Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Live Instances</Typography>
                  <Typography variant="h4" color="success.main">
                    {gatewayMetrics?.live_proxy_instances || 0}
                  </Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Failover Events</Typography>
                  <Typography variant="h4" color="warning.main">
                    {gatewayMetrics?.fallback_events || 0}
                  </Typography>
                </CardContent></Card>
              </Grid>
            </Grid>

            <Card sx={{ mb: 2 }}>
              <CardContent>
                <Typography variant="h6" gutterBottom>
                  <Route sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                  Routing Table
                </Typography>
                <TableContainer component={Paper}>
                  <Table size="small">
                    <TableHead>
                      <TableRow>
                        <TableCell>Gateway Endpoint</TableCell>
                        <TableCell>Selected Backend</TableCell>
                        <TableCell>Engine</TableCell>
                        <TableCell>Latency</TableCell>
                        <TableCell>Health</TableCell>
                        <TableCell>Candidates</TableCell>
                      </TableRow>
                    </TableHead>
                    <TableBody>
                      {(status?.provisioned_ports || []).filter(p => p.alive).map((port, i) => (
                        <TableRow key={i} hover>
                          <TableCell sx={{ fontFamily: 'monospace' }}>:{port.port}</TableCell>
                          <TableCell sx={{ maxWidth: 200, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', fontFamily: 'monospace', fontSize: '0.75rem' }}>
                            {port.uri}
                          </TableCell>
                          <TableCell><Chip label={port.engine_used || '-'} size="small" /></TableCell>
                          <TableCell>{formatLatency(port.latency_ms)}</TableCell>
                          <TableCell><Chip label="HEALTHY" size="small" color="success" /></TableCell>
                          <TableCell>
                            {(status?.alive_configs || []).slice(0, 5).map((c, j) => (
                              <Chip key={j} label={getProtocol(c.uri)} size="small" variant="outlined" sx={{ mr: 0.5, mb: 0.5 }} />
                            ))}
                          </TableCell>
                        </TableRow>
                      ))}
                      {(status?.provisioned_ports || []).filter(p => p.alive).length === 0 && (
                        <TableRow>
                          <TableCell colSpan={6} align="center">
                            <Typography color="text.secondary">No active routes</Typography>
                          </TableCell>
                        </TableRow>
                      )}
                    </TableBody>
                  </Table>
                </TableContainer>
              </CardContent>
            </Card>

            <Card>
              <CardContent>
                <Typography variant="h6" gutterBottom>
                  <Router sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                  Balancer Status
                </Typography>
                <TableContainer component={Paper}>
                  <Table size="small">
                    <TableHead>
                      <TableRow>
                        <TableCell>Port</TableCell>
                        <TableCell>Type</TableCell>
                        <TableCell>Running</TableCell>
                        <TableCell>Backends</TableCell>
                        <TableCell>Healthy</TableCell>
                        <TableCell>TCP</TableCell>
                        <TableCell>SOCKS</TableCell>
                        <TableCell>HTTP</TableCell>
                      </TableRow>
                    </TableHead>
                    <TableBody>
                      {(status?.balancers || []).map((bal, i) => (
                        <TableRow key={i} hover>
                          <TableCell sx={{ fontFamily: 'monospace' }}>{bal.port}</TableCell>
                          <TableCell><Chip label={bal.type} size="small" /></TableCell>
                          <TableCell>
                            <Chip label={bal.running ? 'RUNNING' : 'STOPPED'} size="small" color={bal.running ? 'success' : 'error'} />
                          </TableCell>
                          <TableCell>{bal.backends}</TableCell>
                          <TableCell sx={{ color: 'success.main' }}>{bal.healthy}</TableCell>
                          <TableCell>{bal.tcp_alive ? '✓' : '✗'}</TableCell>
                          <TableCell>{bal.socks_ready ? '✓' : '✗'}</TableCell>
                          <TableCell>{bal.http_ready ? '✓' : '✗'}</TableCell>
                        </TableRow>
                      ))}
                      {(status?.balancers || []).length === 0 && (
                        <TableRow>
                          <TableCell colSpan={8} align="center">
                            <Typography color="text.secondary">No balancers running</Typography>
                          </TableCell>
                        </TableRow>
                      )}
                    </TableBody>
                  </Table>
                </TableContainer>
              </CardContent>
            </Card>
          </Box>
        )}

        {/* === ENGINE STATUS TAB === */}
        {activeTab === 7 && (
          <Box>
            <Grid container spacing={2} sx={{ mb: 2 }}>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Xray</Typography>
                  <Typography variant="h4" color="primary.main">
                    {(status?.provisioned_ports || []).filter(p => p.engine_used === 'xray' && p.alive).length}
                  </Typography>
                  <Typography variant="body2" color="text.secondary">
                    of {(status?.provisioned_ports || []).filter(p => p.engine_used === 'xray').length} provisioned
                  </Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Sing-box</Typography>
                  <Typography variant="h4" color="secondary.main">
                    {(status?.provisioned_ports || []).filter(p => p.engine_used === 'sing-box' && p.alive).length}
                  </Typography>
                  <Typography variant="body2" color="text.secondary">
                    of {(status?.provisioned_ports || []).filter(p => p.engine_used === 'sing-box').length} provisioned
                  </Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Mihomo</Typography>
                  <Typography variant="h4" color="warning.main">
                    {(status?.provisioned_ports || []).filter(p => p.engine_used === 'mihomo' && p.alive).length}
                  </Typography>
                  <Typography variant="body2" color="text.secondary">
                    of {(status?.provisioned_ports || []).filter(p => p.engine_used === 'mihomo').length} provisioned
                  </Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Tor</Typography>
                  <Typography variant="h4" color="info.main">
                    {(status?.provisioned_ports || []).filter(p => p.engine_used === 'tor' && p.alive).length}
                  </Typography>
                  <Typography variant="body2" color="text.secondary">
                    of {(status?.provisioned_ports || []).filter(p => p.engine_used === 'tor').length} provisioned
                  </Typography>
                </CardContent></Card>
              </Grid>
            </Grid>

            <Card>
              <CardContent>
                <Typography variant="h6" gutterBottom>
                  <Memory sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                  Engine Instances
                </Typography>
                <TableContainer component={Paper}>
                  <Table size="small">
                    <TableHead>
                      <TableRow>
                        <TableCell>Port</TableCell>
                        <TableCell>Engine</TableCell>
                        <TableCell>Protocol</TableCell>
                        <TableCell>State</TableCell>
                        <TableCell>Latency</TableCell>
                        <TableCell>URI</TableCell>
                        <TableCell>Actions</TableCell>
                      </TableRow>
                    </TableHead>
                    <TableBody>
                      {(status?.provisioned_ports || []).map((port, i) => (
                        <TableRow key={i} hover>
                          <TableCell sx={{ fontFamily: 'monospace' }}>{port.port}</TableCell>
                          <TableCell>
                            <Chip
                              label={port.engine_used || 'unknown'}
                              size="small"
                              color={
                                port.engine_used === 'xray' ? 'primary' :
                                port.engine_used === 'sing-box' ? 'secondary' :
                                port.engine_used === 'mihomo' ? 'warning' :
                                port.engine_used === 'tor' ? 'info' : 'default'
                              }
                            />
                          </TableCell>
                          <TableCell><Chip label={getProtocol(port.uri)} size="small" variant="outlined" /></TableCell>
                          <TableCell>
                            <Chip
                              label={port.alive ? 'RUNNING' : 'STOPPED'}
                              size="small"
                              color={port.alive ? 'success' : 'error'}
                            />
                          </TableCell>
                          <TableCell>{formatLatency(port.latency_ms)}</TableCell>
                          <TableCell sx={{ maxWidth: 200, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', fontFamily: 'monospace', fontSize: '0.75rem' }}>
                            {port.uri}
                          </TableCell>
                          <TableCell>
                            <Stack direction="row" spacing={0.5}>
                              <Tooltip title="Copy URI">
                                <IconButton size="small" onClick={() => copyToClipboard(port.uri, 'URI')}>
                                  <ContentCopy fontSize="small" />
                                </IconButton>
                              </Tooltip>
                              <Tooltip title="QR Code">
                                <IconButton size="small" onClick={() => showQrCode(port.uri)}>
                                  <QrCode2 fontSize="small" color="primary" />
                                </IconButton>
                              </Tooltip>
                            </Stack>
                          </TableCell>
                        </TableRow>
                      ))}
                      {(status?.provisioned_ports || []).length === 0 && (
                        <TableRow>
                          <TableCell colSpan={7} align="center">
                            <Typography color="text.secondary">No engine instances running</Typography>
                          </TableCell>
                        </TableRow>
                      )}
                    </TableBody>
                  </Table>
                </TableContainer>
              </CardContent>
            </Card>

            <Card sx={{ mt: 2 }}>
              <CardContent>
                <Typography variant="h6" gutterBottom>
                  <Assessment sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                  System Resources
                </Typography>
                <Grid container spacing={2}>
                  <Grid item xs={12} sm={6} md={3}>
                    <Typography variant="body2" color="text.secondary">CPU Usage</Typography>
                    <LinearProgress
                      variant="determinate"
                      value={status?.hardware?.cpu_percent || 0}
                      color={(status?.hardware?.cpu_percent ?? 0) > 80 ? 'error' : (status?.hardware?.cpu_percent ?? 0) > 60 ? 'warning' : 'success'}
                      sx={{ height: 8, borderRadius: 4, mt: 1 }}
                    />
                    <Typography variant="body2">{status?.hardware?.cpu_percent?.toFixed(1) || 0}% ({status?.hardware?.cpu_count || 0} cores)</Typography>
                  </Grid>
                  <Grid item xs={12} sm={6} md={3}>
                    <Typography variant="body2" color="text.secondary">RAM Usage</Typography>
                    <LinearProgress
                      variant="determinate"
                      value={status?.hardware?.ram_percent || 0}
                      color={(status?.hardware?.ram_percent ?? 0) > 80 ? 'error' : (status?.hardware?.ram_percent ?? 0) > 60 ? 'warning' : 'success'}
                      sx={{ height: 8, borderRadius: 4, mt: 1 }}
                    />
                    <Typography variant="body2">
                      {status?.hardware?.ram_used_gb?.toFixed(1) || 0} / {status?.hardware?.ram_total_gb?.toFixed(1) || 0} GB
                    </Typography>
                  </Grid>
                  <Grid item xs={12} sm={6} md={3}>
                    <Typography variant="body2" color="text.secondary">IO Pool</Typography>
                    <Typography variant="body1">
                      {status?.hardware?.io_active || 0} active / {status?.hardware?.io_pending || 0} pending
                    </Typography>
                    <Typography variant="body2" color="text.secondary">Pool size: {status?.hardware?.io_pool_size || 0}</Typography>
                  </Grid>
                  <Grid item xs={12} sm={6} md={3}>
                    <Typography variant="body2" color="text.secondary">CPU Pool</Typography>
                    <Typography variant="body1">
                      {status?.hardware?.cpu_active || 0} active / {status?.hardware?.cpu_pending || 0} pending
                    </Typography>
                    <Typography variant="body2" color="text.secondary">Pool size: {status?.hardware?.cpu_pool_size || 0}</Typography>
                  </Grid>
                </Grid>
              </CardContent>
            </Card>
          </Box>
        )}

        {/* === NOC TAB === */}
        {activeTab === 8 && (
          <Box>
            <Grid container spacing={2} sx={{ mb: 2 }}>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Discovered Configs</Typography>
                  <Typography variant="h4">{status?.db?.total || 0}</Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Validated Configs</Typography>
                  <Typography variant="h4">{status?.db?.tested_unique || 0}</Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Gold (Alive)</Typography>
                  <Typography variant="h4" color="success.main">{status?.db?.alive || 0}</Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Silver (Untested)</Typography>
                  <Typography variant="h4" color="warning.main">{status?.db?.untested_unique || 0}</Typography>
                </CardContent></Card>
              </Grid>
            </Grid>

            <Grid container spacing={2} sx={{ mb: 2 }}>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Active Hosted</Typography>
                  <Typography variant="h4" color="primary.main">
                    {(status?.provisioned_ports || []).filter(p => p.alive).length}
                  </Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Routed Sessions</Typography>
                  <Typography variant="h4" color="info.main">
                    {gatewayMetrics?.active_sessions || 0}
                  </Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Bandwidth Proxied</Typography>
                  <Typography variant="h4">
                    {gatewayMetrics?.total_bytes_proxied > 1048576
                      ? `${(gatewayMetrics.total_bytes_proxied / 1048576).toFixed(1)} MB`
                      : gatewayMetrics?.total_bytes_proxied > 1024
                        ? `${(gatewayMetrics.total_bytes_proxied / 1024).toFixed(1)} KB`
                        : `${gatewayMetrics?.total_bytes_proxied || 0} B`}
                  </Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Failover Events</Typography>
                  <Typography variant="h4" color="warning.main">
                    {gatewayMetrics?.fallback_events || 0}
                  </Typography>
                </CardContent></Card>
              </Grid>
            </Grid>

            <Grid container spacing={2} sx={{ mb: 2 }}>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Avg Latency</Typography>
                  <Typography variant="h4">{formatLatency(status?.db?.avg_latency_ms || 0)}</Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">P95 Latency</Typography>
                  <Typography variant="h4">
                    {gatewayMetrics?.p95_latency_ms ? `${gatewayMetrics.p95_latency_ms}ms` : '-'}
                  </Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Failure Rate</Typography>
                  <Typography variant="h4" color={
                    (gatewayMetrics?.failure_rate || 0) > 0.1 ? 'error.main' :
                    (gatewayMetrics?.failure_rate || 0) > 0.05 ? 'warning.main' : 'success.main'
                  }>
                    {((gatewayMetrics?.failure_rate || 0) * 100).toFixed(1)}%
                  </Typography>
                </CardContent></Card>
              </Grid>
              <Grid item xs={12} sm={6} md={3}>
                <Card><CardContent>
                  <Typography color="text.secondary" variant="body2">Pass Rate</Typography>
                  <Typography variant="h4" color="success.main">
                    {status?.db?.total_tests
                      ? ((status.db.total_passes / status.db.total_tests) * 100).toFixed(1)
                      : 0}%
                  </Typography>
                </CardContent></Card>
              </Grid>
            </Grid>

            <Card sx={{ mb: 2 }}>
              <CardContent>
                <Typography variant="h6" gutterBottom>
                  <TrendingUp sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                  Top Performing Configs
                </Typography>
                <TableContainer component={Paper}>
                  <Table size="small">
                    <TableHead>
                      <TableRow>
                        <TableCell>#</TableCell>
                        <TableCell>URI</TableCell>
                        <TableCell>Protocol</TableCell>
                        <TableCell>Engine</TableCell>
                        <TableCell>Latency</TableCell>
                        <TableCell>Pass Rate</TableCell>
                        <TableCell>Tests</TableCell>
                        <TableCell>Actions</TableCell>
                      </TableRow>
                    </TableHead>
                    <TableBody>
                      {(status?.alive_configs || [])
                        .slice()
                        .sort((a, b) => a.latency_ms - b.latency_ms)
                        .slice(0, 20)
                        .map((cfg, i) => (
                        <TableRow key={i} hover>
                          <TableCell>{i + 1}</TableCell>
                          <TableCell sx={{ maxWidth: 250, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', fontFamily: 'monospace', fontSize: '0.75rem' }}>
                            {cfg.uri}
                          </TableCell>
                          <TableCell><Chip label={getProtocol(cfg.uri)} size="small" color="primary" variant="outlined" /></TableCell>
                          <TableCell><Chip label={cfg.engine_used || '-'} size="small" /></TableCell>
                          <TableCell>{formatLatency(cfg.latency_ms)}</TableCell>
                          <TableCell>
                            {cfg.total_tests > 0
                              ? `${((cfg.total_passes / cfg.total_tests) * 100).toFixed(0)}%`
                              : '-'}
                          </TableCell>
                          <TableCell>{cfg.total_tests}</TableCell>
                          <TableCell>
                            <Stack direction="row" spacing={0.5}>
                              <IconButton size="small" onClick={() => copyToClipboard(cfg.uri, 'URI')}>
                                <ContentCopy fontSize="small" />
                              </IconButton>
                              <IconButton size="small" onClick={() => showQrCode(cfg.uri)}>
                                <QrCode2 fontSize="small" color="primary" />
                              </IconButton>
                            </Stack>
                          </TableCell>
                        </TableRow>
                      ))}
                      {(status?.alive_configs || []).length === 0 && (
                        <TableRow>
                          <TableCell colSpan={8} align="center">
                            <Typography color="text.secondary">No alive configs</Typography>
                          </TableCell>
                        </TableRow>
                      )}
                    </TableBody>
                  </Table>
                </TableContainer>
              </CardContent>
            </Card>

            <Card>
              <CardContent>
                <Typography variant="h6" gutterBottom>
                  <MonitorHeart sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                  Health Trends
                </Typography>
                <Grid container spacing={2}>
                  <Grid item xs={12} md={6}>
                    <Typography variant="body2" color="text.secondary" gutterBottom>Validation Rate</Typography>
                    <LinearProgress
                      variant="determinate"
                      value={status?.db?.total ? (status.db.tested_unique / status.db.total) * 100 : 0}
                      color="primary"
                      sx={{ height: 10, borderRadius: 5 }}
                    />
                    <Typography variant="body2" sx={{ mt: 0.5 }}>
                      {status?.db?.tested_unique || 0} / {status?.db?.total || 0} tested
                    </Typography>
                  </Grid>
                  <Grid item xs={12} md={6}>
                    <Typography variant="body2" color="text.secondary" gutterBottom>Alive Rate</Typography>
                    <LinearProgress
                      variant="determinate"
                      value={status?.db?.tested_unique ? (status.db.alive / status.db.tested_unique) * 100 : 0}
                      color="success"
                      sx={{ height: 10, borderRadius: 5 }}
                    />
                    <Typography variant="body2" sx={{ mt: 0.5 }}>
                      {status?.db?.alive || 0} alive of {status?.db?.tested_unique || 0} tested
                    </Typography>
                  </Grid>
                  <Grid item xs={12} md={6}>
                    <Typography variant="body2" color="text.secondary" gutterBottom>Active Proxy Rate</Typography>
                    <LinearProgress
                      variant="determinate"
                      value={
                        (status?.provisioned_ports?.length ?? 0) > 0
                          ? ((status?.provisioned_ports?.filter(p => p.alive).length ?? 0) / (status?.provisioned_ports?.length ?? 1)) * 100
                          : 0
                      }
                      color="secondary"
                      sx={{ height: 10, borderRadius: 5 }}
                    />
                    <Typography variant="body2" sx={{ mt: 0.5 }}>
                      {status?.provisioned_ports?.filter(p => p.alive).length || 0} / {status?.provisioned_ports?.length || 0} active
                    </Typography>
                  </Grid>
                  <Grid item xs={12} md={6}>
                    <Typography variant="body2" color="text.secondary" gutterBottom>Gateway Health</Typography>
                    <LinearProgress
                      variant="determinate"
                      value={100 - ((gatewayMetrics?.failure_rate || 0) * 100)}
                      color={gatewayMetrics?.failure_rate > 0.1 ? 'error' : 'success'}
                      sx={{ height: 10, borderRadius: 5 }}
                    />
                    <Typography variant="body2" sx={{ mt: 0.5 }}>
                      Failure rate: {((gatewayMetrics?.failure_rate || 0) * 100).toFixed(1)}%
                    </Typography>
                  </Grid>
                </Grid>
              </CardContent>
            </Card>

            {/* WebSocket Connection Stats */}
            <Card sx={{ mb: 2 }}>
              <CardContent>
                <Typography variant="h6" gutterBottom>
                  <Hub sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                  WebSocket Connections
                </Typography>
                <Grid container spacing={2} sx={{ mb: 2 }}>
                  <Grid item xs={12} sm={6} md={3}>
                    <Card variant="outlined"><CardContent>
                      <Typography color="text.secondary" variant="body2">Monitor Clients</Typography>
                      <Typography variant="h4" color="primary.main">
                        {wsStats?.active_monitor_clients ?? status?.ws_stats?.active_monitor_clients ?? 0}
                      </Typography>
                    </CardContent></Card>
                  </Grid>
                  <Grid item xs={12} sm={6} md={3}>
                    <Card variant="outlined"><CardContent>
                      <Typography color="text.secondary" variant="body2">Control Clients</Typography>
                      <Typography variant="h4" color="info.main">
                        {wsStats?.active_control_clients ?? status?.ws_stats?.active_control_clients ?? 0}
                      </Typography>
                    </CardContent></Card>
                  </Grid>
                  <Grid item xs={12} sm={6} md={3}>
                    <Card variant="outlined"><CardContent>
                      <Typography color="text.secondary" variant="body2">Total Connections</Typography>
                      <Typography variant="h4">
                        {wsStats?.total_connections ?? status?.ws_stats?.total_connections ?? 0}
                      </Typography>
                    </CardContent></Card>
                  </Grid>
                  <Grid item xs={12} sm={6} md={3}>
                    <Card variant="outlined"><CardContent>
                      <Typography color="text.secondary" variant="body2">Total Disconnections</Typography>
                      <Typography variant="h4" color="warning.main">
                        {wsStats?.total_disconnections ?? status?.ws_stats?.total_disconnections ?? 0}
                      </Typography>
                    </CardContent></Card>
                  </Grid>
                </Grid>
                <Grid container spacing={2}>
                  <Grid item xs={12} sm={6} md={3}>
                    <Card variant="outlined"><CardContent>
                      <Typography color="text.secondary" variant="body2">Messages Sent</Typography>
                      <Typography variant="h5">
                        {wsStats?.messages_sent ?? status?.ws_stats?.messages_sent ?? 0}
                      </Typography>
                    </CardContent></Card>
                  </Grid>
                  <Grid item xs={12} sm={6} md={3}>
                    <Card variant="outlined"><CardContent>
                      <Typography color="text.secondary" variant="body2">Messages Received</Typography>
                      <Typography variant="h5">
                        {wsStats?.messages_received ?? status?.ws_stats?.messages_received ?? 0}
                      </Typography>
                    </CardContent></Card>
                  </Grid>
                  <Grid item xs={12} sm={6} md={3}>
                    <Card variant="outlined"><CardContent>
                      <Typography color="text.secondary" variant="body2">Pings Sent</Typography>
                      <Typography variant="h5">
                        {wsStats?.pings_sent ?? status?.ws_stats?.pings_sent ?? 0}
                      </Typography>
                    </CardContent></Card>
                  </Grid>
                  <Grid item xs={12} sm={6} md={3}>
                    <Card variant="outlined"><CardContent>
                      <Typography color="text.secondary" variant="body2">Reconnect Count</Typography>
                      <Typography variant="h5" color="warning.main">{reconnectCount}</Typography>
                    </CardContent></Card>
                  </Grid>
                </Grid>
              </CardContent>
            </Card>

            {/* Active Upstreams */}
            <Card sx={{ mb: 2 }}>
              <CardContent>
                <Typography variant="h6" gutterBottom>
                  <Dns sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                  Active Upstream Tunnels
                </Typography>
                <TableContainer component={Paper}>
                  <Table size="small">
                    <TableHead>
                      <TableRow>
                        <TableCell>Name</TableCell>
                        <TableCell>Host</TableCell>
                        <TableCell>SOCKS Port</TableCell>
                        <TableCell>Status</TableCell>
                        <TableCell>Latency</TableCell>
                        <TableCell>Reconnects</TableCell>
                        <TableCell>Preferred</TableCell>
                      </TableRow>
                    </TableHead>
                    <TableBody>
                      {(upstreamTunnels || []).map((up, i) => (
                        <TableRow key={i} hover>
                          <TableCell>{up.name}</TableCell>
                          <TableCell sx={{ fontFamily: 'monospace', fontSize: '0.75rem' }}>{up.host}</TableCell>
                          <TableCell>{up.port}</TableCell>
                          <TableCell>
                            <Chip
                              label={up.connected ? 'Connected' : 'Disconnected'}
                              color={up.connected ? 'success' : 'error'}
                              size="small"
                            />
                          </TableCell>
                          <TableCell>{up.latency_ms ? `${up.latency_ms}ms` : '-'}</TableCell>
                          <TableCell>{up.reconnect_count || 0}</TableCell>
                          <TableCell>{up.preferred ? <CheckCircle color="primary" fontSize="small" /> : '-'}</TableCell>
                        </TableRow>
                      ))}
                      {(upstreamTunnels || []).length === 0 && (
                        <TableRow>
                          <TableCell colSpan={7} align="center">
                            <Typography color="text.secondary">No upstream tunnels</Typography>
                          </TableCell>
                        </TableRow>
                      )}
                    </TableBody>
                  </Table>
                </TableContainer>
              </CardContent>
            </Card>

            {/* Generated Configs Status */}
            <Card>
              <CardContent>
                <Typography variant="h6" gutterBottom>
                  <Widgets sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                  Generated Config Pool Status
                </Typography>
                <Grid container spacing={2} sx={{ mb: 2 }}>
                  <Grid item xs={12} sm={6} md={3}>
                    <Card variant="outlined"><CardContent>
                      <Typography color="text.secondary" variant="body2">Total Configs</Typography>
                      <Typography variant="h4">{generatedConfigs.length || status?.generated_total || 0}</Typography>
                    </CardContent></Card>
                  </Grid>
                  <Grid item xs={12} sm={6} md={3}>
                    <Card variant="outlined"><CardContent>
                      <Typography color="text.secondary" variant="body2">Active Configs</Typography>
                      <Typography variant="h4" color="success.main">
                        {(generatedConfigs.filter(c => c.active).length) || status?.generated_active || 0}
                      </Typography>
                    </CardContent></Card>
                  </Grid>
                  <Grid item xs={12} sm={6} md={3}>
                    <Card variant="outlined"><CardContent>
                      <Typography color="text.secondary" variant="body2">Gold Pool</Typography>
                      <Typography variant="h4" color="warning.main">{status?.db?.alive || 0}</Typography>
                    </CardContent></Card>
                  </Grid>
                  <Grid item xs={12} sm={6} md={3}>
                    <Card variant="outlined"><CardContent>
                      <Typography color="text.secondary" variant="body2">Silver Pool</Typography>
                      <Typography variant="h4">{status?.db?.untested_unique || 0}</Typography>
                    </CardContent></Card>
                  </Grid>
                </Grid>
                <TableContainer component={Paper}>
                  <Table size="small">
                    <TableHead>
                      <TableRow>
                        <TableCell>Name</TableCell>
                        <TableCell>Port</TableCell>
                        <TableCell>Upstream</TableCell>
                        <TableCell>Status</TableCell>
                        <TableCell>Latency</TableCell>
                        <TableCell>URI</TableCell>
                        <TableCell>Actions</TableCell>
                      </TableRow>
                    </TableHead>
                    <TableBody>
                      {(generatedConfigs.length > 0 ? generatedConfigs : (status?.generated_configs || [])).map((cfg, i) => (
                        <TableRow key={i} hover>
                          <TableCell><Chip label={cfg.name} size="small" color="primary" variant="outlined" /></TableCell>
                          <TableCell>{cfg.port}</TableCell>
                          <TableCell>{cfg.upstream || '-'}</TableCell>
                          <TableCell>
                            <Chip
                              label={cfg.active ? 'Active' : 'Inactive'}
                              color={cfg.active ? 'success' : 'error'}
                              size="small"
                            />
                          </TableCell>
                          <TableCell>{cfg.latency_ms ? `${cfg.latency_ms}ms` : '-'}</TableCell>
                          <TableCell sx={{ fontFamily: 'monospace', fontSize: '0.7rem', maxWidth: 200, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                            {cfg.uri || `${cfg.protocol || 'socks5'}://${cfg.host || 'api.abharcable.com'}:${cfg.port}#${cfg.name}`}
                          </TableCell>
                          <TableCell>
                            <Stack direction="row" spacing={0.5}>
                              <IconButton size="small" onClick={() => {
                                const uri = cfg.uri || `${cfg.protocol || 'socks5'}://${cfg.host || 'api.abharcable.com'}:${cfg.port}#${cfg.name}`
                                copyToClipboard(uri, cfg.name)
                              }}>
                                <ContentCopy fontSize="small" />
                              </IconButton>
                              <IconButton size="small" onClick={() => {
                                const uri = cfg.uri || `${cfg.protocol || 'socks5'}://${cfg.host || 'api.abharcable.com'}:${cfg.port}#${cfg.name}`
                                showQrCode(uri)
                              }}>
                                <QrCode2 fontSize="small" color="primary" />
                              </IconButton>
                            </Stack>
                          </TableCell>
                        </TableRow>
                      ))}
                      {generatedConfigs.length === 0 && (status?.generated_configs || []).length === 0 && (
                        <TableRow>
                          <TableCell colSpan={7} align="center">
                            <Typography color="text.secondary">No generated configs</Typography>
                          </TableCell>
                        </TableRow>
                      )}
                    </TableBody>
                  </Table>
                </TableContainer>
              </CardContent>
            </Card>
          </Box>
        )}

        {/* === WORKERS TAB === */}
        {activeTab === 9 && (
          <Box>
            <Card>
              <CardContent>
                <Typography variant="h6" gutterBottom>
                  <People sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                  Worker Threads
                </Typography>
                <TableContainer component={Paper}>
                  <Table size="small">
                    <TableHead>
                      <TableRow>
                        <TableCell>Name</TableCell>
                        <TableCell>State</TableCell>
                        <TableCell>Runs</TableCell>
                        <TableCell>Errors</TableCell>
                        <TableCell>Last Run</TableCell>
                        <TableCell>Next Run</TableCell>
                        <TableCell>Last Error</TableCell>
                      </TableRow>
                    </TableHead>
                    <TableBody>
                      {(status?.workers || []).map((w) => (
                        <TableRow key={w.name} hover>
                          <TableCell sx={{ fontWeight: 500 }}>{w.name}</TableCell>
                          <TableCell>
                            <Chip label={w.state} size="small" color={stateColors[w.state] || 'default'} />
                          </TableCell>
                          <TableCell>{w.runs}</TableCell>
                          <TableCell>
                            {w.errors > 0 ? <Chip label={w.errors} size="small" color="error" /> : w.errors}
                          </TableCell>
                          <TableCell>{formatTs(w.last_run)}</TableCell>
                          <TableCell>
                            {w.next_run_in > 0 ? `${w.next_run_in}s` : '-'}
                          </TableCell>
                          <TableCell sx={{ maxWidth: 250, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', color: 'error.main', fontSize: '0.75rem' }}>
                            {w.last_error || '-'}
                          </TableCell>
                        </TableRow>
                      ))}
                      {(status?.workers || []).length === 0 && (
                        <TableRow>
                          <TableCell colSpan={7} align="center">
                            <Typography color="text.secondary">No workers running</Typography>
                          </TableCell>
                        </TableRow>
                      )}
                    </TableBody>
                  </Table>
                </TableContainer>
              </CardContent>
            </Card>

            {/* Worker Extra Details */}
            {(status?.workers || []).filter(w => Object.keys(w.extra || {}).length > 0).length > 0 && (
              <Card sx={{ mt: 2 }}>
                <CardContent>
                  <Typography variant="h6" gutterBottom>Worker Details</Typography>
                  <Grid container spacing={2}>
                    {(status?.workers || []).filter(w => Object.keys(w.extra || {}).length > 0).map((w) => (
                      <Grid item xs={12} md={6} key={w.name}>
                        <Paper variant="outlined" sx={{ p: 2 }}>
                          <Typography variant="subtitle2" gutterBottom>{w.name}</Typography>
                          <Grid container spacing={1}>
                            {Object.entries(w.extra).slice(0, 8).map(([k, v]) => (
                              <Grid item xs={6} key={k}>
                                <Typography variant="caption" color="text.secondary">{k}</Typography>
                                <Typography variant="body2" sx={{ fontSize: '0.75rem' }}>{v}</Typography>
                              </Grid>
                            ))}
                          </Grid>
                        </Paper>
                      </Grid>
                    ))}
                  </Grid>
                </CardContent>
              </Card>
            )}
          </Box>
        )}

        {/* === CONTROL TAB === */}
        {activeTab === 10 && (
          <Box>
            <Grid container spacing={2}>
              {/* Main Control */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>
                      <Dashboard sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Main Control
                    </Typography>
                    <Stack spacing={2}>
                      <ButtonGroup variant="contained" fullWidth>
                        <Button
                          startIcon={<PlayArrow />}
                          color="success"
                          onClick={() => sendCommand('start')}
                          disabled={!connected}
                        >
                          Start
                        </Button>
                        <Button
                          startIcon={<Pause />}
                          color="warning"
                          onClick={() => sendCommand('pause')}
                          disabled={!connected}
                        >
                          Pause
                        </Button>
                        <Button
                          startIcon={<PlayArrow />}
                          color="info"
                          onClick={() => sendCommand('resume')}
                          disabled={!connected}
                        >
                          Resume
                        </Button>
                        <Button
                          startIcon={<Stop />}
                          color="error"
                          onClick={() => sendCommand('stop')}
                          disabled={!connected}
                        >
                          Stop
                        </Button>
                      </ButtonGroup>
                      <Button
                        variant="outlined"
                        startIcon={<Refresh />}
                        onClick={() => sendCommand('run_cycle')}
                        disabled={!connected}
                      >
                        Run Single Cycle
                      </Button>
                    </Stack>
                  </CardContent>
                </Card>
              </Grid>

              {/* Speed Control */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>
                      <Speed sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Speed Profile
                    </Typography>
                    <Stack spacing={2}>
                      <ToggleButtonGroup
                        value={speedProfile}
                        exclusive
                        onChange={(_, v) => { if (v) { setSpeedProfile(v); sendCommand('speed_profile', { value: v }) } }}
                        fullWidth
                        size="small"
                      >
                        <ToggleButton value="low">Low</ToggleButton>
                        <ToggleButton value="medium">Medium</ToggleButton>
                        <ToggleButton value="high">High</ToggleButton>
                        <ToggleButton value="turbo">Turbo</ToggleButton>
                      </ToggleButtonGroup>
                      <Grid container spacing={1}>
                        <Grid item xs={6}>
                          <TextField
                            label="Max Threads"
                            type="number"
                            size="small"
                            defaultValue={status?.speed?.max_threads || 10}
                            onBlur={(e) => sendCommand('set_threads', { value: parseInt(e.target.value, 10) })}
                            InputProps={{ inputProps: { min: 1, max: 50 } }}
                            fullWidth
                          />
                        </Grid>
                        <Grid item xs={6}>
                          <TextField
                            label="Timeout (s)"
                            type="number"
                            size="small"
                            defaultValue={status?.speed?.test_timeout_s || 5}
                            onBlur={(e) => sendCommand('set_timeout', { value: parseInt(e.target.value, 10) })}
                            InputProps={{ inputProps: { min: 1, max: 10 } }}
                            fullWidth
                          />
                        </Grid>
                      </Grid>
                      <Typography variant="caption" color="text.secondary">
                        Current: {status?.speed?.max_threads || 0} threads, {status?.speed?.test_timeout_s || 0}s timeout, {status?.speed?.effective_max_concurrent || 0} concurrent
                      </Typography>
                    </Stack>
                  </CardContent>
                </Card>
              </Grid>

              {/* Port Management */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>
                      <Router sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Port Management
                    </Typography>
                    <Stack spacing={1}>
                      <Button variant="outlined" startIcon={<Refresh />} onClick={() => sendCommand('refresh_ports')}>
                        Refresh Ports
                      </Button>
                      <Button variant="outlined" startIcon={<Speed />} onClick={() => sendCommand('recheck_live_ports')}>
                        Recheck Live Ports
                      </Button>
                      <Button variant="outlined" color="warning" startIcon={<Build />} onClick={() => sendCommand('reprovision_ports')}>
                        Reprovision All Ports
                      </Button>
                    </Stack>
                  </CardContent>
                </Card>
              </Grid>

              {/* Censorship Tools */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>
                      <Science sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Censorship & Network Tools
                    </Typography>
                    <Stack spacing={1}>
                      <Button variant="outlined" startIcon={<TravelExplore />} onClick={() => sendCommand('detect_censorship')}>
                        Detect Censorship
                      </Button>
                      <Button variant="outlined" startIcon={<TravelExplore />} onClick={() => sendCommand('discover_exit_ip')}>
                        Discover Exit IP
                      </Button>
                      <Button variant="outlined" startIcon={<Security />} onClick={() => sendCommand('edge_router_bypass')}>
                        Toggle Edge Router Bypass
                      </Button>
                    </Stack>
                    {status?.censorship && (
                      <Alert severity={status.censorship.network_type === 'censored' ? 'warning' : 'success'} sx={{ mt: 1 }}>
                        Network: {status.censorship.network_type} | Strategy: {status.censorship.strategy}
                      </Alert>
                    )}
                  </CardContent>
                </Card>
              </Grid>

              {/* Config Management */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>
                      <Dns sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Config Management
                    </Typography>
                    <Stack spacing={2}>
                      <Box>
                        <Typography variant="body2" gutterBottom>Clear old configs (hours):</Typography>
                        <Stack direction="row" spacing={1}>
                          <TextField
                            type="number"
                            size="small"
                            value={clearHours}
                            onChange={(e) => setClearHours(parseInt(e.target.value, 10) || 168)}
                            sx={{ width: 100 }}
                          />
                          <Button variant="outlined" color="warning" startIcon={<Delete />} onClick={() => sendCommand('clear_old', { hours: clearHours })}>
                            Clear Old
                          </Button>
                        </Stack>
                      </Box>
                      <Button variant="outlined" color="error" startIcon={<Delete />} onClick={() => sendCommand('clear_alive')}>
                        Clear Alive Configs
                      </Button>
                      <Button variant="outlined" startIcon={<CloudUpload />} onClick={() => sendCommand('load_raw_files')}>
                        Load Raw Config Files
                      </Button>
                      <Button variant="outlined" startIcon={<CloudUpload />} onClick={() => sendCommand('load_bundle_files')}>
                        Load Bundle Files
                      </Button>
                    </Stack>
                  </CardContent>
                </Card>
              </Grid>

              {/* Download */}
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>
                      <Download sx={{ mr: 1, verticalAlign: 'middle', fontSize: 20 }} />
                      Download & Export
                    </Typography>
                    <Stack spacing={1}>
                      <Button variant="contained" color="success" startIcon={<Download />} onClick={() => downloadConfigs('gold')}>
                        Download Gold Configs
                      </Button>
                      <Button variant="contained" color="warning" startIcon={<Download />} onClick={() => downloadConfigs('silver')}>
                        Download Silver Configs
                      </Button>
                      <Button variant="contained" startIcon={<Download />} onClick={() => downloadConfigs('all')}>
                        Download All Configs
                      </Button>
                      <Button variant="outlined" startIcon={<Download />} onClick={downloadStatus}>
                        Export Status JSON
                      </Button>
                      <Button variant="outlined" startIcon={<CloudUpload />} onClick={() => sendCommand('export_config_db')}>
                        Export Config DB
                      </Button>
                    </Stack>
                  </CardContent>
                </Card>
              </Grid>
            </Grid>
          </Box>
        )}

        {/* === SETTINGS TAB === */}
        {activeTab === 11 && (
          <Box>
            <Grid container spacing={2}>
              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>Runtime Configuration</Typography>
                    <Grid container spacing={2}>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Main Proxy Port</Typography>
                        <Typography variant="body1" sx={{ fontFamily: 'monospace' }}>{status?.runtime_config?.multiproxy_port || 7803}</Typography>
                      </Grid>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Gemini Proxy Port</Typography>
                        <Typography variant="body1" sx={{ fontFamily: 'monospace' }}>{status?.runtime_config?.gemini_port || 7804}</Typography>
                      </Grid>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Max Total</Typography>
                        <Typography variant="body1">{status?.runtime_config?.max_total || '-'}</Typography>
                      </Grid>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Max Workers</Typography>
                        <Typography variant="body1">{status?.runtime_config?.max_workers || '-'}</Typography>
                      </Grid>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Scan Limit</Typography>
                        <Typography variant="body1">{status?.runtime_config?.scan_limit || '-'}</Typography>
                      </Grid>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Sleep Seconds</Typography>
                        <Typography variant="body1">{status?.runtime_config?.sleep_seconds || '-'}</Typography>
                      </Grid>
                    </Grid>
                  </CardContent>
                </Card>
              </Grid>

              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>Engine Paths</Typography>
                    <Stack spacing={1}>
                      <TextField label="XRay Path" size="small" defaultValue={status?.runtime_config?.xray_path || ''} fullWidth />
                      <TextField label="SingBox Path" size="small" defaultValue={status?.runtime_config?.singbox_path || ''} fullWidth />
                      <TextField label="Mihomo Path" size="small" defaultValue={status?.runtime_config?.mihomo_path || ''} fullWidth />
                      <TextField label="Tor Path" size="small" defaultValue={status?.runtime_config?.tor_path || ''} fullWidth />
                      <Button
                        variant="contained"
                        startIcon={<SettingsIcon />}
                        onClick={() => {
                          const inputs = document.querySelectorAll('input[label]')
                          const settings: Record<string, string> = {}
                          inputs.forEach(inp => {
                            const el = inp as HTMLInputElement
                            settings[el.getAttribute('aria-label') || ''] = el.value
                          })
                          sendCommand('update_runtime_settings', settings)
                        }}
                      >
                        Save Settings
                      </Button>
                    </Stack>
                  </CardContent>
                </Card>
              </Grid>

              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>Telegram Settings</Typography>
                    <Grid container spacing={2}>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Enabled</Typography>
                        <Chip label={status?.runtime_config?.telegram_enabled ? 'Yes' : 'No'} color={status?.runtime_config?.telegram_enabled ? 'success' : 'default'} size="small" />
                      </Grid>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Limit</Typography>
                        <Typography variant="body1">{status?.runtime_config?.telegram_limit || 0}</Typography>
                      </Grid>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Timeout (ms)</Typography>
                        <Typography variant="body1">{status?.runtime_config?.telegram_timeout_ms || 0}</Typography>
                      </Grid>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">Targets</Typography>
                        <Typography variant="body1">{status?.runtime_config?.targets_count || 0}</Typography>
                      </Grid>
                      <Grid item xs={6}>
                        <Typography variant="body2" color="text.secondary">GitHub URLs</Typography>
                        <Typography variant="body1">{status?.runtime_config?.github_urls_count || 0}</Typography>
                      </Grid>
                    </Grid>
                  </CardContent>
                </Card>
              </Grid>

              <Grid item xs={12} md={6}>
                <Card>
                  <CardContent>
                    <Typography variant="h6" gutterBottom>Connection Info</Typography>
                    <List dense>
                      <ListItem>
                        <ListItemText primary="Backend Host" secondary={hostname} />
                      </ListItem>
                      <ListItem>
                        <ListItemText primary="Status WebSocket" secondary={`ws://${hostname}:${process.env.NEXT_PUBLIC_WS_STATUS_PORT || '7802'}`} />
                      </ListItem>
                      <ListItem>
                        <ListItemText primary="Command WebSocket" secondary={`ws://${hostname}:${process.env.NEXT_PUBLIC_WS_COMMAND_PORT || '7801'}`} />
                      </ListItem>
                      <ListItem>
                        <ListItemText primary="Main SOCKS5" secondary={`${hostname}:${status?.runtime_config?.multiproxy_port || 7803}`} />
                      </ListItem>
                      <ListItem>
                        <ListItemText primary="Gemini SOCKS5" secondary={`${hostname}:${status?.runtime_config?.gemini_port || 7804}`} />
                      </ListItem>
                    </List>
                  </CardContent>
                </Card>
              </Grid>
            </Grid>
          </Box>
        )}
      </Container>

      {/* Add Configs Dialog */}
      <Dialog open={showAddDialog} onClose={() => setShowAddDialog(false)} maxWidth="md" fullWidth>
        <DialogTitle>Add Configurations</DialogTitle>
        <DialogContent>
          <DialogContentText sx={{ mb: 2 }}>
            Paste configuration URIs (one per line). Supported: vmess://, vless://, trojan://, ss://, ssr://, etc.
          </DialogContentText>
          <TextField
            multiline
            rows={10}
            fullWidth
            value={addConfigText}
            onChange={(e) => setAddConfigText(e.target.value)}
            placeholder="vmess://..."
            variant="outlined"
          />
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setShowAddDialog(false)}>Cancel</Button>
          <Button variant="contained" onClick={handleAddConfigs} disabled={!addConfigText.trim()}>
            Add Configs
          </Button>
        </DialogActions>
      </Dialog>

      {/* QR Code Dialog */}
      <Dialog
        open={qrDialogOpen}
        onClose={() => setQrDialogOpen(false)}
        maxWidth="sm"
        fullWidth
      >
        <DialogTitle>
          <Stack direction="row" alignItems="center" spacing={1}>
            <QrCode2 color="primary" />
            <Typography variant="h6">QR Code - Scan to Import</Typography>
          </Stack>
        </DialogTitle>
        <DialogContent>
          <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', py: 2 }}>
            {qrDataUrl && (
              <Box
                component="img"
                src={qrDataUrl}
                alt="QR Code"
                sx={{
                  width: 400,
                  height: 400,
                  bgcolor: 'white',
                  p: 2,
                  borderRadius: 2,
                }}
              />
            )}
            <Typography variant="body2" color="text.secondary" sx={{ mt: 2, textAlign: 'center' }}>
              Scan with your V2Ray/Xray client to import this configuration
            </Typography>
            <Typography
              variant="caption"
              sx={{
                mt: 1,
                fontFamily: 'monospace',
                wordBreak: 'break-all',
                maxWidth: '100%',
                color: 'text.secondary',
                textAlign: 'center',
              }}
            >
              {qrUri.substring(0, 100)}{qrUri.length > 100 ? '...' : ''}
            </Typography>
          </Box>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => copyToClipboard(qrUri, 'URI')} startIcon={<ContentCopy />}>
            Copy URI
          </Button>
          <Button onClick={() => downloadSingleConfig(qrUri)} startIcon={<Download />}>
            Download
          </Button>
          <Button onClick={() => setQrDialogOpen(false)}>Close</Button>
        </DialogActions>
      </Dialog>

      {/* Config Detail Dialog */}
      <Dialog
        open={detailDialogOpen}
        onClose={() => setDetailDialogOpen(false)}
        maxWidth="md"
        fullWidth
      >
        <DialogTitle>
          <Stack direction="row" alignItems="center" spacing={1}>
            <Visibility color="primary" />
            <Typography variant="h6">Configuration Details</Typography>
          </Stack>
        </DialogTitle>
        <DialogContent>
          {detailConfig && (
            <Box sx={{ py: 1 }}>
              <Grid container spacing={2}>
                <Grid item xs={12}>
                  <Typography variant="body2" color="text.secondary">Protocol</Typography>
                  <Chip label={getProtocol(detailConfig.uri)} color="primary" />
                </Grid>
                <Grid item xs={12}>
                  <Typography variant="body2" color="text.secondary">URI</Typography>
                  <Paper sx={{ p: 1.5, bgcolor: 'background.default', maxHeight: 200, overflow: 'auto' }}>
                    <Typography variant="body2" sx={{ fontFamily: 'monospace', wordBreak: 'break-all' }}>
                      {detailConfig.uri}
                    </Typography>
                  </Paper>
                </Grid>
                <Grid item xs={6} sm={3}>
                  <Typography variant="body2" color="text.secondary">Engine</Typography>
                  <Typography variant="body1">{detailConfig.engine_used || '-'}</Typography>
                </Grid>
                <Grid item xs={6} sm={3}>
                  <Typography variant="body2" color="text.secondary">Latency</Typography>
                  <Typography variant="body1">{formatLatency(detailConfig.latency_ms)}</Typography>
                </Grid>
                <Grid item xs={6} sm={3}>
                  <Typography variant="body2" color="text.secondary">Tag</Typography>
                  <Typography variant="body1">{detailConfig.tag || '-'}</Typography>
                </Grid>
                <Grid item xs={6} sm={3}>
                  <Typography variant="body2" color="text.secondary">Alive</Typography>
                  <Chip label={detailConfig.alive ? 'YES' : 'NO'} size="small" color={detailConfig.alive ? 'success' : 'error'} />
                </Grid>
                <Grid item xs={6} sm={3}>
                  <Typography variant="body2" color="text.secondary">Total Tests</Typography>
                  <Typography variant="body1">{detailConfig.total_tests}</Typography>
                </Grid>
                <Grid item xs={6} sm={3}>
                  <Typography variant="body2" color="text.secondary">Total Passes</Typography>
                  <Typography variant="body1" color="success.main">{detailConfig.total_passes}</Typography>
                </Grid>
                <Grid item xs={6} sm={3}>
                  <Typography variant="body2" color="text.secondary">Consecutive Fails</Typography>
                  <Typography variant="body1" color={detailConfig.consecutive_fails > 3 ? 'error.main' : 'inherit'}>
                    {detailConfig.consecutive_fails}
                  </Typography>
                </Grid>
                <Grid item xs={6} sm={3}>
                  <Typography variant="body2" color="text.secondary">First Seen</Typography>
                  <Typography variant="body1">{formatTs(detailConfig.first_seen)}</Typography>
                </Grid>
                <Grid item xs={6} sm={3}>
                  <Typography variant="body2" color="text.secondary">Last Alive</Typography>
                  <Typography variant="body1">{formatTs(detailConfig.last_alive)}</Typography>
                </Grid>
                <Grid item xs={6} sm={3}>
                  <Typography variant="body2" color="text.secondary">Last Tested</Typography>
                  <Typography variant="body1">{formatTs(detailConfig.last_tested)}</Typography>
                </Grid>
                <Grid item xs={6} sm={3}>
                  <Typography variant="body2" color="text.secondary">Telegram Only</Typography>
                  <Typography variant="body1">{detailConfig.telegram_only ? 'YES' : 'NO'}</Typography>
                </Grid>
              </Grid>
            </Box>
          )}
        </DialogContent>
        <DialogActions>
          <Button onClick={() => detailConfig && copyToClipboard(detailConfig.uri, 'URI')} startIcon={<ContentCopy />}>
            Copy URI
          </Button>
          <Button onClick={() => detailConfig && showQrCode(detailConfig.uri)} startIcon={<QrCode2 />}>
            QR Code
          </Button>
          <Button onClick={() => detailConfig && downloadSingleConfig(detailConfig.uri)} startIcon={<Download />}>
            Download
          </Button>
          <Button onClick={() => setDetailDialogOpen(false)}>Close</Button>
        </DialogActions>
      </Dialog>

      {/* Snackbar */}
      <Snackbar
        open={snackbar.open}
        autoHideDuration={4000}
        onClose={() => setSnackbar({ ...snackbar, open: false })}
        anchorOrigin={{ vertical: 'bottom', horizontal: 'right' }}
      >
        <Alert
          onClose={() => setSnackbar({ ...snackbar, open: false })}
          severity={snackbar.severity}
          variant="filled"
        >
          {snackbar.msg}
        </Alert>
      </Snackbar>
    </Box>
  )
}
