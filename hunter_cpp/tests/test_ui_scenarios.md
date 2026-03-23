# Test Scenarios for Hunter Censorship UI

## 1. Copy Operations Testing

### Test Case 1.1: Copy All Healthy (Empty State)
**Steps:**
1. Start huntercensor with no configs
2. Navigate to Home page
3. Click "Copy All Healthy" button

**Expected Results:**
- Warning toast: "No healthy configs to copy"
- Log entry: "[UI] Copy All Healthy: no healthy configs available"
- No clipboard change

### Test Case 1.2: Copy All Healthy (With Duplicates)
**Steps:**
1. Load configs with duplicate URIs
2. Navigate to Home page
3. Observe the "(X total, Y unique)" text
4. Click "Copy All Healthy" button

**Expected Results:**
- Success toast with deduplication percentage: "Copied Y healthy configs (XX.X% dedup)"
- Log entry with dedup stats: "[UI] Copy All Healthy: requested=X unique=Y dedup=XX.X% success=true"
- Clipboard contains deduplicated URIs

### Test Case 1.3: Copy All Live/Visible (Filter Test)
**Steps:**
1. Load mix of alive and dead configs
2. Navigate to Configs page
3. Toggle between "Live Only" and "All"
4. Click "Copy All Live" or "Copy All Visible"

**Expected Results:**
- Correct count based on filter
- Proper deduplication statistics
- Toast shows appropriate count

### Test Case 1.4: Copy All Port URIs
**Steps:**
1. Start huntercensor with provisioned ports
2. Navigate to Advanced > Provisioned Ports
3. Click "Copy All Port URIs"

**Expected Results:**
- Shows deduplication stats if duplicate ports exist
- Clipboard contains unique port URIs

### Test Case 1.5: Clipboard Error Handling
**Steps:**
1. Simulate clipboard failure (e.g., by opening another app that locks clipboard)
2. Attempt any copy operation

**Expected Results:**
- Error toast: "Failed to copy [item]"
- Detailed error log with Windows error code
- No application crash

## 2. Progress Indicators Testing

### Test Case 2.1: Probe Censorship Progress
**Steps:**
1. Navigate to Censorship page
2. Click "Probe Censorship"
3. Observe button state and progress text

**Expected Results:**
- Button becomes disabled
- "Probing censorship..." text appears in amber
- Progress completes with success/error toast
- Progress logged with timing

### Test Case 2.2: Discover Exit IP Progress
**Steps:**
1. Navigate to Censorship page
2. Click "Discover Exit IP"
3. Monitor progress indicator

**Expected Results:**
- Button disabled during operation
- "Discovering exit IP..." text shown
- Discovery logs appear in real-time
- Completion updates UI state

### Test Case 2.3: Execute Bypass Progress
**Steps:**
1. Complete discovery first
2. Click "Execute Bypass"
3. Observe long-running operation

**Expected Results:**
- Progress indicator shows "Executing edge bypass..."
- Real-time logs in Edge Router Bypass Log section
- Success/failure toast on completion
- 30-second timeout handling

### Test Case 2.4: Concurrent Operations
**Steps:**
1. Start probe operation
2. Try to start discovery before probe completes

**Expected Results:**
- Only one operation runs at a time
- Buttons properly disabled/enabled
- No UI freezing or crashes

## 3. Error Handling Testing

### Test Case 3.1: Network Failure During Probe
**Steps:**
1. Disconnect network
2. Attempt probe operation

**Expected Results:**
- Error toast with meaningful message
- Detailed error in logs
- UI remains responsive

### Test Case 3.2: Invalid Configuration
**Steps:**
1. Provide invalid XRay path in Advanced settings
2. Try to start huntercensor

**Expected Results:**
- Clear error message
- Application doesn't crash
- Can recover by fixing path

### Test Case 3.3: Memory Pressure
**Steps:**
1. Load large number of configs (>100,000)
2. Perform copy operations

**Expected Results:**
- Operations complete successfully
- Memory usage stays reasonable
- No slowdown or crashes

## 4. UI Responsiveness Testing

### Test Case 4.1: Background Operations
**Steps:**
1. Start long-running operation (probe/discovery)
2. Navigate between pages
3. Interact with other UI elements

**Expected Results:**
- UI remains fully responsive
- No blocking or freezing
- Smooth transitions between pages

### Test Case 4.2: Rapid Click Testing
**Steps:**
1. Rapidly click buttons multiple times
2. Toggle switches quickly
3. Resize window rapidly

**Expected Results:**
- No duplicate operations queued
- UI state remains consistent
- No crashes or hangs

## 5. Data Integrity Testing

### Test Case 5.1: Config Deduplication
**Steps:**
1. Create test file with duplicate configs
2. Load into huntercensor
3. Verify deduplication across all copy operations

**Expected Results:**
- Consistent deduplication everywhere
- Order preserved (first occurrence kept)
- Accurate statistics

### Test Case 5.2: Special Characters in URIs
**Steps:**
1. Load configs with special characters
2. Test copy operations

**Expected Results:**
- Special characters preserved
- No corruption in clipboard
- Proper UTF-8 handling

## 6. Performance Testing

### Test Case 6.1: Large Dataset Copy
**Steps:**
1. Load 50,000+ configs
2. Copy all healthy configs

**Expected Results:**
- Operation completes within 2 seconds
- UI remains responsive
- Memory usage stable

### Test Case 6.2: Real-time Log Performance
**Steps:**
1. Generate high volume of logs
2. Monitor Logs page performance

**Expected Results:**
- Smooth scrolling
- No memory leaks
- 1000-line cap enforced

## 7. Accessibility Testing

### Test Case 7.1: Color Contrast
**Steps:**
1. Verify all text has sufficient contrast
2. Check color blind friendliness

**Expected Results:**
- All text readable
- Information not conveyed by color alone

### Test Case 7.2: Keyboard Navigation
**Steps:**
1. Navigate UI using only keyboard
2. Test tab order and shortcuts

**Expected Results:**
- All elements reachable
- Logical tab order
- Clear focus indicators

## 8. Integration Testing

### Test Case 8.1: GitHub Background Refresh
**Steps:**
1. Monitor GitHub refresh logs
2. Verify status updates in UI

**Expected Results:**
- Detailed logging as implemented
- Status fields updated correctly
- Error conditions handled

### Test Case 8.2: Provisioned Ports Integration
**Steps:**
1. Start huntercensor
2. Check provisioned ports status
3. Verify port URIs are accessible

**Expected Results:**
- Ports show correct status
- URIs are valid and copyable
- Statistics accurate

## Test Execution Checklist

- [ ] Clear logs before each test
- [ ] Document actual results vs expected
- [ ] Capture screenshots for UI tests
- [ ] Monitor memory usage during performance tests
- [ ] Verify error messages are user-friendly
- [ ] Test on different screen resolutions
- [ ] Verify DPI scaling works correctly
- [ ] Check for memory leaks after extended use

## Regression Testing

After each code change, run:
1. All copy operations tests
2. Progress indicator tests
3. Error handling tests
4. Basic performance tests

This ensures new features don't break existing functionality.
