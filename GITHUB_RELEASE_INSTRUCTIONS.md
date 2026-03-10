# GitHub Release Instructions for Hunter v1.0.0

## 🚀 Step-by-Step Release Process

### 1. Push to GitHub (Manual due to network issues)
```bash
# Since direct push fails from Iran, use one of these methods:
# Method 1: VPN/Proxy
git push main master

# Method 2: GitHub Desktop (if available)
# Open GitHub Desktop and sync the repository

# Method 3: GitHub web interface
# Go to https://github.com/bahmany/censorship_hunter
# Click "Add file" -> "Upload files"
# Upload the modified files
```

### 2. Create GitHub Release

1. **Open Release Page**: https://github.com/bahmany/censorship_hunter/releases/new

2. **Fill Release Details**:
   - **Tag**: `v1.0.0`
   - **Target**: `master`
   - **Release Title**: `Hunter v1.0.0 - Complete Anti-Censorship Solution`
   - **Description**: Copy content from `RELEASE_NOTES.md`

3. **Upload Assets**:
   - Click "Attach binaries"
   - Upload: `Hunter-v1.0.0-Final.zip` (64.55 MB)

4. **Publish**:
   - Check "Set as the latest release"
   - Click "Publish release"

### 3. Verify Release

1. **Check Download**: https://github.com/bahmany/censorship_hunter/releases/download/v1.0.0/Hunter-v1.0.0-Final.zip

2. **Test Extraction**: Extract ZIP and verify all files are present

3. **Test Application**: Run `hunter_dashboard.exe` and verify it works

## 📋 Release Checklist

- [ ] Code pushed to GitHub
- [ ] Tag v1.0.0 created
- [ ] Release created with proper title
- [ ] Release notes copied from RELEASE_NOTES.md
- [ ] Hunter-v1.0.0-Final.zip uploaded (64.55 MB)
- [ ] Release set as "latest"
- [ ] Download link tested
- [ ] Installation tested on clean system

## 🔗 Important Links

- **Repository**: https://github.com/bahmany/censorship_hunter
- **Release Page**: https://github.com/bahmany/censorship_hunter/releases
- **Direct Download**: https://github.com/bahmany/censorship_hunter/releases/download/v1.0.0/Hunter-v1.0.0-Final.zip
- **Tag**: https://github.com/bahmany/censorship_hunter/tree/v1.0.0

## 📊 Release Statistics

- **Version**: v1.0.0
- **Size**: 64.55 MB
- **Files**: 50+ files including all dependencies
- **Platform**: Windows x64
- **Dependencies**: None (completely self-contained)
- **Testing**: Verified in multiple isolated environments

## 🎯 Post-Release Tasks

1. **Announce on Telegram** (if applicable)
2. **Update README.md** with download link
3. **Create GitHub Discussions** for user support
4. **Monitor Issues** for bug reports
5. **Prepare v1.0.1** for any hotfixes

## 🚨 Troubleshooting

### If Push Fails:
- Use VPN/proxy to bypass Iranian network restrictions
- Use GitHub Desktop application
- Upload files manually via web interface

### If Release Upload Fails:
- Check file size (GitHub limit: 2GB per file)
- Ensure stable internet connection
- Try uploading during off-peak hours

### If Download Link Doesn't Work:
- Wait a few minutes for GitHub to process the release
- Check the release page for any errors
- Verify the file was uploaded successfully

---

**Release Status**: Ready for publication
**Next Step**: Manual push and GitHub release creation
