# Hunter Docker Deployment Guide

This guide explains how to deploy Hunter using Docker and Docker Compose for server-based operation with a web-based UI.

## Architecture

The Docker deployment consists of two main services:

1. **Hunter Backend** (C++): The core proxy discovery and load balancing engine
   - Exposes WebSocket control port (8080) for commands
   - Exposes WebSocket monitor port (8081) for real-time status updates
   - Provides SOCKS5 proxy endpoints (10808, 10809)
   - Provisioned proxy ports (2901-2999)

2. **Hunter Web UI** (Next.js/React): Modern web interface for monitoring and control
   - Connects to backend via WebSocket
   - Real-time status updates
   - Control panel for start/stop/pause/resume
   - Configuration database visualization

## Prerequisites

- Docker Engine 20.10+
- Docker Compose 2.0+
- At least 2GB RAM available
- 10GB free disk space

## Quick Start

### 1. Build and Start Services

```bash
# Clone the repository
git clone https://github.com/bahmany/censorship_hunter.git
cd censorship_hunter

# Build and start all services
docker-compose up -d

# View logs
docker-compose logs -f
```

### 2. Access the Web UI

Open your browser and navigate to:
```
http://localhost:3000
```

### 3. Configure Your Browser

Use the SOCKS5 proxy endpoint:
```
Proxy: 127.0.0.1
Port: 10808
```

## Configuration

### Environment Variables

Backend environment variables (in `docker-compose.yml`):

```yaml
HUNTER_REST_API_PORT=8080        # WebSocket control port
HUNTER_WS_MONITOR_PORT=8081      # WebSocket monitor port
HUNTER_MULTIPROXY_PORT=10808     # Main SOCKS5 proxy port
HUNTER_GEMINI_PORT=10809          # Secondary SOCKS5 proxy port
```

Web UI environment variables:

```yaml
NEXT_PUBLIC_BACKEND_URL=ws://hunter-backend:8081
NEXT_PUBLIC_API_URL=http://hunter-backend:8080
```

### Volumes

Persistent data is stored in Docker volumes:
- `hunter-runtime`: Runtime configuration and state
- `hunter-config`: Configuration files
- `hunter-logs`: Application logs

## Service Management

### Start Services
```bash
docker-compose up -d
```

### Stop Services
```bash
docker-compose down
```

### View Logs
```bash
# All services
docker-compose logs -f

# Backend only
docker-compose logs -f hunter-backend

# Web UI only
docker-compose logs -f hunter-web-ui
```

### Restart Services
```bash
docker-compose restart
```

### Rebuild After Changes
```bash
docker-compose up -d --build
```

## Ports Exposed

| Port | Service | Description |
|------|---------|-------------|
| 3000 | Web UI | Next.js web interface |
| 8080 | Backend | WebSocket control API |
| 8081 | Backend | WebSocket monitor (real-time status) |
| 10808 | Backend | Main SOCKS5 proxy |
| 10809 | Backend | Gemini SOCKS5 proxy |
| 2901-2999 | Backend | Provisioned proxy ports |

## Health Checks

The backend service includes a health check:
```bash
curl http://localhost:8080/health
```

## Troubleshooting

### Backend Not Starting

Check logs for errors:
```bash
docker-compose logs hunter-backend
```

Common issues:
- Port conflicts: Ensure ports 8080, 8081, 10808, 10809 are available
- Missing dependencies: Rebuild the Docker image
- Permission issues: Check Docker volume permissions

### Web UI Cannot Connect to Backend

1. Verify backend is running:
```bash
docker-compose ps
```

2. Check network connectivity:
```bash
docker-compose exec hunter-web-ui ping hunter-backend
```

3. Verify WebSocket ports:
```bash
docker-compose exec hunter-backend netstat -tlnp | grep 808
```

### Proxy Not Working

1. Check if backend is running cycles
2. Verify gold configs are available in the web UI
3. Check browser proxy configuration
4. Test with curl:
```bash
curl --socks5 127.0.0.1:10808 https://www.google.com
```

## Building Individual Components

### Backend Only
```bash
docker build -f Dockerfile -t hunter-backend .
```

### Web UI Only
```bash
cd web-ui
docker build -f Dockerfile -t hunter-web-ui .
```

## Production Deployment

### Using Reverse Proxy (Nginx)

Create an `nginx.conf`:
```nginx
server {
    listen 80;
    server_name hunter.example.com;

    location / {
        proxy_pass http://localhost:3000;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection 'upgrade';
        proxy_set_header Host $host;
        proxy_cache_bypass $http_upgrade;
    }

    location /ws {
        proxy_pass http://localhost:8081;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
    }
}
```

### Security Considerations

1. **Do not expose proxy ports publicly**: Only expose the web UI (3000) and control API (8080) behind authentication
2. **Use HTTPS**: Deploy behind a reverse proxy with SSL/TLS
3. **Network isolation**: Use Docker networks to isolate services
4. **Resource limits**: Add resource limits to docker-compose.yml:
```yaml
services:
  hunter-backend:
    deploy:
      resources:
        limits:
          cpus: '2'
          memory: 2G
```

## Monitoring

### View Resource Usage
```bash
docker stats
```

### View Container Status
```bash
docker-compose ps
```

## Backup and Restore

### Backup Volumes
```bash
docker run --rm -v hunter-runtime:/data -v $(pwd):/backup ubuntu tar czf /backup/runtime-backup.tar.gz /data
docker run --rm -v hunter-config:/data -v $(pwd):/backup ubuntu tar czf /backup/config-backup.tar.gz /data
```

### Restore Volumes
```bash
docker run --rm -v hunter-runtime:/data -v $(pwd):/backup ubuntu tar xzf /backup/runtime-backup.tar.gz -C /
docker run --rm -v hunter-config:/data -v $(pwd):/backup ubuntu tar xzf /backup/config-backup.tar.gz -C /
```

## Development

### Running Backend Locally (Linux)

```bash
cd hunter_cpp
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja
./hunter_backend
```

### Running Web UI Locally

```bash
cd web-ui
npm install
npm run dev
```

## Support

For issues and questions:
- GitHub Issues: https://github.com/bahmany/censorship_hunter/issues
- Documentation: See README.md in the repository
