# Service Accessibility Feature - Implementation Guide

## Summary

This document describes the new Service Accessibility feature added to Censorship Hunter. This feature enables tracking and filtering proxy configurations based on their accessibility to five key online services: Gemini, Claude, Devin, Google Developer Console, and AWS Console.

## Files Added

### 1. Web Dashboard
- **`web-accessibility-dashboard.html`** (1201 lines)
  - Fully functional HTML/CSS/JavaScript dashboard
  - Real-time filtering and search capabilities
  - Statistics display
  - Data export to CSV
  - Auto-refresh functionality
  - Responsive design with dark theme
  - Mock data generator for testing
  - Integration-ready API endpoints

### 2. Backend C++ Code

#### Headers
- **`hunter_cpp/include/network/service_accessibility.h`** (195 lines)
  - `ServiceAccessibilityChecker` class for testing service accessibility
  - `ServiceAccessibilityDatabase` class for managing service status data
  - Support for 5 services: Gemini, Claude, Devin, Google Developer, AWS Console
  - Status enum: UNKNOWN, BLOCKED, ACCESSIBLE, LOCATION_FORBIDDEN

- **`hunter_cpp/include/realtime/accessibility_api.h`** (130 lines)
  - `AccessibilityAPI` class for WebSocket command handling
  - JSON-RPC style commands
  - Filtering, statistics, export functionality

#### Implementation
- **`hunter_cpp/src/network/service_accessibility.cpp`** (265 lines)
  - Service accessibility checker implementation
  - HTTP status code parsing
  - Geographic restriction detection
  - Database operations

- **`hunter_cpp/src/realtime/accessibility_api.cpp`** (346 lines)
  - WebSocket API command handlers
  - JSON request/response formatting
  - Filtering logic
  - CSV export generation

#### Tests
- **`hunter_cpp/tests/test_service_accessibility.cpp`** (144 lines)
  - Unit tests for ServiceAccessibilityChecker
  - Unit tests for ServiceAccessibilityDatabase
  - Integration tests
  - API command handler tests (template)

### 3. Documentation
- **`ACCESSIBILITY_FEATURE.md`** (390 lines)
  - Comprehensive feature documentation
  - User interface description
  - Backend integration guide
  - API endpoint specifications
  - Testing checklist
  - Troubleshooting guide
  - Future enhancements

- **`SERVICE_ACCESSIBILITY_IMPLEMENTATION.md`** (this file)
  - Implementation overview
  - File listing
  - Integration steps
  - Testing instructions
  - Deployment guide

## Model Changes

### Modified: `hunter_cpp/include/core/models.h`

Added new fields to `ConfigHealthRecord` struct:

```cpp
// Service accessibility tracking
// -1 = unknown/not checked, 0 = blocked, 1 = accessible, 2 = location_forbidden

// Gemini (Google AI)
int gemini_status = -1;
double gemini_checked_at = 0.0;

// Claude (Anthropic AI)
int claude_status = -1;
double claude_checked_at = 0.0;

// Devin (AI Coding Assistant)
int devin_status = -1;
double devin_checked_at = 0.0;

// Google Developer Console
int google_developer_status = -1;
double google_developer_checked_at = 0.0;

// AWS Console
int aws_console_status = -1;
double aws_console_checked_at = 0.0;
```

Note: The `gemini_status` field already existed in the codebase, so the change extends it with four additional service fields.

## Integration Steps

### 1. Update CMakeLists.txt

Add the new source files to your build:

```cmake
# In hunter_cpp/CMakeLists.txt, add to the target_sources for hunter_core:

src/network/service_accessibility.cpp
src/realtime/accessibility_api.cpp
```

### 2. Update Main Application

If using a WebSocket server, add the new API handler:

```cpp
#include "realtime/accessibility_api.h"

// In your WebSocket message handler:
AccessibilityAPI accessibility_api;
json response = accessibility_api.handleCommand(incoming_json_command);
```

### 3. Database Integration

Integrate with existing ConfigDatabase:

```cpp
// In continuous_validator.h/cpp:
void testAllServices(const std::string& uri, ConfigHealthRecord& record);
void updateAllServiceStatuses(const ConfigHealthRecord& record);
```

### 4. Web Server Configuration

Serve the dashboard HTML:

```nginx
# In nginx config
location /dashboard/accessibility {
    alias /path/to/web-accessibility-dashboard.html;
}
```

Or if using a web server in your app:

```cpp
// Serve the HTML file with appropriate MIME type
http_server.serve("GET", "/dashboard", "text/html", html_content);
```

## Testing

### Unit Tests

```bash
cd hunter_cpp
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make test_service_accessibility
./tests/test_service_accessibility
```

### Integration Tests

1. **Build the project:**
```bash
cd hunter_cpp
mkdir -p build && cd build
cmake .. -G Ninja
ninja
```

2. **Run the application:**
```bash
./bin/hunter_backend
```

3. **Open the dashboard:**
- Save `web-accessibility-dashboard.html` to your web root
- Open in browser: `http://localhost:8080/accessibility-dashboard.html`
- Or use the goose app preview created during development

4. **Test filters:**
- [ ] Text search by URI/hash
- [ ] Status filter
- [ ] Source filter
- [ ] Latency filter
- [ ] Service combination filters
- [ ] Pagination
- [ ] Column sorting
- [ ] CSV export
- [ ] Auto-refresh

### API Testing

```bash
# Test via curl (if WebSocket API available)
curl -X POST http://localhost:7801/api/accessibility \
  -H 'Content-Type: application/json' \
  -d '{
    "method": "get_configs_with_services",
    "params": {
      "page": 1,
      "page_size": 25
    }
  }'

# Test filter
curl -X POST http://localhost:7801/api/accessibility \
  -H 'Content-Type: application/json' \
  -d '{
    "method": "filter_by_services",
    "params": {
      "services": ["gemini", "claude"],
      "require_all": false,
      "page": 1
    }
  }'
```

## Performance Considerations

### Data Volume
- Dashboard tested with 500+ mock configurations
- Client-side filtering for instant results
- Pagination supports 10-100 rows per page

### Optimization Tips
1. For datasets > 10k configs: implement server-side filtering
2. Cache service test results for 1 hour minimum
3. Batch service tests to reduce API calls
4. Use WebWorkers for large-scale data processing

### Recommended Hardware
- Minimum: 2GB RAM, 2-core CPU
- Recommended: 4GB RAM, 4-core CPU for 10k+ configs

## Deployment

### Docker Deployment

1. **Add to docker-compose.yml:**

```yaml
services:
  hunter-backend:
    # ... existing config ...
    volumes:
      - ./web-accessibility-dashboard.html:/app/dashboard/accessibility.html

  hunter-web-ui:
    # Can serve the dashboard via Next.js or Nginx
```

2. **Nginx Configuration:**

```nginx
location /api/accessibility {
    proxy_pass http://hunter-backend:7801;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
}

location /dashboard/accessibility {
    alias /app/dashboard/accessibility.html;
}
```

### Standalone Deployment

1. **Copy files:**
```bash
cp web-accessibility-dashboard.html /var/www/html/
cp hunter_cpp/include/network/service_accessibility.h /usr/local/include/
cp hunter_cpp/lib/libhunter_core.a /usr/local/lib/
```

2. **Update application:**
```cpp
#include <service_accessibility.h>

// Initialize in main()
AccessibilityAPI api;
```

## Features Implemented

✅ **Service Tracking**
- Gemini accessibility
- Claude accessibility
- Devin accessibility
- Google Developer Console accessibility
- AWS Console accessibility

✅ **Filtering**
- Text search (URI, hash, source)
- Status filtering
- Source filtering
- Latency filtering
- Service combination filtering

✅ **Statistics**
- Total configurations count
- Accessible configurations count
- Location forbidden count
- Blocked count
- Average latency

✅ **Data Management**
- Sortable columns
- Pagination (10, 25, 50, 100 per page)
- CSV export
- Auto-refresh

✅ **UI/UX**
- Dark theme with blue accents
- Color-coded status badges
- Latency visualization
- Detail modal
- Responsive design

## API Endpoints (WebSocket)

### 1. Get Configs with Services
```json
{
  "method": "get_configs_with_services",
  "params": {
    "page": 1,
    "page_size": 25,
    "sort_by": "uri",
    "sort_order": "asc"
  }
}
```

### 2. Filter by Services
```json
{
  "method": "filter_by_services",
  "params": {
    "services": ["gemini", "claude"],
    "require_all": false,
    "status": "accessible",
    "source": "github",
    "max_latency": 500,
    "page": 1,
    "page_size": 25
  }
}
```

### 3. Get Statistics
```json
{
  "method": "get_accessibility_stats",
  "params": {}
}
```

### 4. Get Services with Access
```json
{
  "method": "get_services_with_access",
  "params": {}
}
```

### 5. Export Configs
```json
{
  "method": "export_configs",
  "params": {
    "services": ["gemini"],
    "format": "csv"
  }
}
```

## Future Enhancements

1. **Real-time Updates**
   - WebSocket live data streaming
   - Automatic refresh on config changes
   - Push notifications for status changes

2. **Analytics**
   - Service accessibility trends
   - Geographic distribution analysis
   - Latency trends over time

3. **User Management**
   - Multi-user support
   - Custom filter presets
   - Saved configurations

4. **Advanced Testing**
   - Custom test URLs per service
   - Parallel service testing
   - Test result caching

5. **Integration**
   - Telegram notifications
   - Slack integration
   - API key management

## Known Limitations

1. Service testing requires actual HTTP requests (currently mocked)
2. No persistent storage of historical data (yet)
3. Dashboard assumes backend API available
4. No user authentication in dashboard (add via Nginx)

## Troubleshooting

### Dashboard shows no data
- Check backend connection
- Verify API endpoint is correct
- Check browser console for errors

### Filters not working
- Clear browser cache
- Verify backend returns correct JSON
- Check service names match

### Slow performance with many configs
- Reduce page size
- Use server-side filtering for > 10k configs
- Check network latency to backend

## Support

For issues or questions:
1. Check ACCESSIBILITY_FEATURE.md for detailed documentation
2. Review test files for usage examples
3. Check browser console for JavaScript errors
4. Verify backend API responses

## License

Same as Censorship Hunter (MIT License)

## Version

- Feature Version: 1.0
- Date: November 2024
- Backend Support: Requires custom WebSocket API handler
- Frontend: Standalone HTML (no dependencies)

## Changelog

### v1.0 (Initial Release)
- Added 5-service accessibility tracking
- Implemented filtering system
- Created web dashboard
- Added C++ backend modules
- Comprehensive documentation
- Unit test suite
