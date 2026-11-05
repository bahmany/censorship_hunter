# Service Accessibility Dashboard - Feature Documentation

## Overview

The Service Accessibility Dashboard is a comprehensive web interface for monitoring and filtering proxy configurations based on their accessibility to various online services. This feature allows users to identify which proxies work with specific services that may be geographically or administratively restricted.

## Features

### 1. Service Accessibility Tracking
Track accessibility status for five key services:
- **Gemini** - Google's AI service
- **Claude** - Anthropic's AI assistant
- **Devin** - AI coding assistant
- **Google Developer** - Google Cloud Developer Console
- **AWS Console** - Amazon Web Services Management Console

### 2. Status Categories
Each service can have one of four statuses:
- **✓ Accessible** - The service is fully accessible through this proxy
- **✗ Blocked** - The service is completely blocked/inaccessible
- **⛔ Location Forbidden** - The service blocks access from this geographic region
- **? Unknown** - The service hasn't been tested yet

### 3. Filtering Capabilities

#### Text Search
- Search by configuration hash
- Search by source URI
- Search by source type

#### Status Filters
- Filter by overall proxy status
- Filter by service accessibility individually

#### Advanced Filters
- **Max Latency** - Only show proxies below a certain latency threshold
- **Source Type** - Filter by GitHub, Telegram, Harvested, or Scraped configs
- **Service Combination** - Filter to show only proxies accessible to specific services

### 4. Data Management

#### Statistics Dashboard
- Total configurations loaded
- Accessible configurations count
- Location forbidden count
- Blocked configuration count
- Average latency across all configs

#### Table Features
- Sortable columns (click headers to sort)
- Pagination (10, 25, 50, or 100 items per page)
- Color-coded status badges
- Latency visualization with progress bars
- Action buttons for each row

### 5. Export & Integration

#### Data Export
- Export filtered results as CSV
- Includes all fields: hash, URI, source, latency, status, and service accessibility

#### Auto-Refresh
- Set auto-refresh interval in seconds
- Useful for monitoring live configuration changes

## User Interface

### Layout
1. **Header** - Project title and description
2. **Control Panel** - Filters and action buttons
3. **Statistics** - Real-time metrics
4. **Data Table** - Paginated configuration list
5. **Detail Modal** - Full configuration information

### Color Scheme
- **Dark theme** - Slate background with blue accents
- **Status indicators** - Green (accessible), Red (blocked), Orange (forbidden), Gray (unknown)
- **Interactive elements** - Hover effects and transitions for better UX

## Backend Integration

### Expected API Response Format

The dashboard expects configuration data in the following format:

```json
{
  "configs": [
    {
      "id": 1,
      "uri": "vless://config@example.com:443",
      "hash": "ek1:abc123...",
      "source": "github",
      "latency": 150,
      "status": "accessible",
      "alive": true,
      "timestamp": 1699000000000,
      "gemini": "accessible",
      "claude": "accessible",
      "devin": "location_forbidden",
      "google_developer": "blocked",
      "aws_console": "accessible"
    }
  ]
}
```

### Status Values
- `accessible` - Service is fully accessible
- `blocked` - Service returns connection refused/timeout
- `location_forbidden` - Service returns 451/403 geographic restriction
- `unknown` - Service not yet tested (default)

### Backend Endpoints to Implement

#### 1. Fetch All Configurations
```
GET /api/configs
Response: 
{
  "configs": [...],
  "total": number,
  "timestamp": ISO string
}
```

#### 2. Fetch Configurations with Filters
```
POST /api/configs/filter
Body:
{
  "services": ["gemini", "claude"],  // Filter by specific services
  "status": "accessible",             // Filter by status
  "source": "github",                 // Filter by source
  "maxLatency": 500,                  // Max latency in ms
  "search": "string"                  // Search term
}
Response: Same as above
```

#### 3. Get Service Statistics
```
GET /api/stats
Response:
{
  "total": number,
  "accessible": number,
  "blocked": number,
  "location_forbidden": number,
  "avgLatency": number,
  "timestamp": ISO string
}
```

#### 4. Update Service Accessibility
```
POST /api/configs/{id}/service-status
Body:
{
  "service": "gemini",
  "status": "accessible",
  "latency": 150
}
Response: { "success": true, "updated": {...} }
```

## Integration with Existing Backend

### Modify `ConfigHealthRecord` (hunter_cpp/include/core/models.h)

Add new fields to track service accessibility:

```cpp
struct ConfigHealthRecord {
    // ... existing fields ...
    
    // Service accessibility tracking
    struct ServiceStatus {
        int status = -1;           // -1=unknown, 0=blocked, 1=accessible, 2=location_forbidden
        double checked_at = 0.0;   // Timestamp of last check
        float latency_ms = 0.0f;   // Latency to this service specifically
    };
    
    ServiceStatus gemini;
    ServiceStatus claude;
    ServiceStatus devin;
    ServiceStatus google_developer;
    ServiceStatus aws_console;
};
```

### Modify Validator (hunter_cpp/src/network/continuous_validator.cpp)

Add methods to test service accessibility:

```cpp
// Test if a proxy can access a specific service
void ConfigDatabase::testServiceAccessibility(
    const std::string& uri,
    const std::string& service_name,
    int& status_out,
    float& latency_out);

// Batch test multiple services
void ConfigDatabase::testAllServices(
    const std::string& uri,
    ConfigHealthRecord& record);

// Get configs accessible to specific service(s)
std::vector<ConfigHealthRecord> ConfigDatabase::getAccessibleFor(
    const std::vector<std::string>& services);
```

### Modify WebSocket Bridge (hunter_cpp/src/realtime/websocket_bridge.cpp)

Add new command handlers:

```cpp
// Get all configs with service status
void handleGetConfigsWithServices(const json& params);

// Filter configs by service accessibility
void handleFilterByServices(const json& params);

// Trigger service accessibility tests
void handleTestServices(const json& params);
```

## Testing & Verification

### Unit Tests
```bash
# Test data model
g++ -std=c++17 -o test_service_status test_service_status.cpp
./test_service_status

# Test filtering logic
g++ -std=c++17 -o test_filters test_filters.cpp
./test_filters
```

### Integration Tests
1. Start the backend
2. Open the dashboard in a browser
3. Verify data loads correctly
4. Test each filter combination
5. Verify CSV export includes all fields
6. Test auto-refresh functionality

### Manual Testing Checklist
- [ ] Dashboard loads without errors
- [ ] All filters work individually
- [ ] Combined filters work correctly
- [ ] Search finds configs by URI/hash/source
- [ ] Pagination works smoothly
- [ ] Column sorting works
- [ ] Stats update when filters change
- [ ] Detail modal shows correct info
- [ ] Copy to clipboard works
- [ ] CSV export includes all rows
- [ ] Auto-refresh works and respects interval
- [ ] Responsive design on mobile

## Performance Considerations

### Data Volume
- Dashboard can handle 500+ configurations efficiently
- Pagination prevents rendering all rows at once
- Filtering is performed client-side for instant results

### Optimization Tips
1. Use backend filtering for very large datasets (>10k configs)
2. Implement server-side pagination for best performance
3. Cache service test results for 1 hour minimum
4. Batch service tests to avoid overwhelming upstream services

## Future Enhancements

1. **Real-time Updates**
   - WebSocket connection for live data updates
   - Show when configs become available/unavailable

2. **Advanced Analytics**
   - Service accessibility trends over time
   - Geographic distribution of accessible services
   - Service-specific latency graphs

3. **User Preferences**
   - Save filter presets
   - Bookmark favorite configurations
   - Custom notifications for status changes

4. **Multi-User Support**
   - User accounts and authentication
   - Shared filter presets
   - Access control by service type

5. **API Management**
   - Generate API keys for programmatic access
   - Rate limiting
   - Webhook notifications

## Configuration

### Environment Variables
```bash
# Backend connection
BACKEND_HOST=localhost
WS_PORT=7802
API_PORT=7801

# Display settings
ITEMS_PER_PAGE=25
AUTO_REFRESH_INTERVAL=0

# Features
ENABLE_EXPORT=true
ENABLE_AUTO_REFRESH=true
ENABLE_DETAIL_VIEW=true
```

### Local Storage (Browser)
- `accessibility_filters` - Last applied filters
- `accessibility_page_size` - Preferred pagination size
- `accessibility_sort_order` - Last sort column and direction

## Troubleshooting

### Dashboard shows no data
1. Check backend connection (verify API endpoint)
2. Verify backend is running and accessible
3. Check browser console for errors
4. Verify API returns correct JSON format

### Filters don't work
1. Clear browser cache
2. Check browser console for JavaScript errors
3. Verify all filter fields have valid values
4. Try resetting filters

### Export creates empty file
1. Verify data is loaded (check table)
2. Check browser download permissions
3. Try with fewer rows (test pagination first)
4. Check browser console for errors

### Auto-refresh not working
1. Verify interval is > 0
2. Check browser console for fetch errors
3. Verify backend is responding
4. Try smaller interval (test with 5 seconds)

## API Usage Examples

### JavaScript/Fetch
```javascript
// Get all configs
const response = await fetch('/api/configs');
const data = await response.json();

// Filter configs
const filtered = await fetch('/api/configs/filter', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({
    services: ['gemini', 'claude'],
    status: 'accessible'
  })
});
```

### cURL
```bash
# Get all configs
curl http://localhost:7801/api/configs

# Filter by service
curl -X POST http://localhost:7801/api/configs/filter \
  -H 'Content-Type: application/json' \
  -d '{
    "services": ["gemini"],
    "status": "accessible"
  }'
```

## License
Same as Censorship Hunter project (MIT License)

## Support
For issues or feature requests, please create an issue on GitHub or contact the maintainers.
