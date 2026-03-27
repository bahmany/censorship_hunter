#pragma once
#include <string>

// FontAwesome 6 Free Solid Icons
// Icon definitions as UTF-8 strings

#define ICON_FA_EYE     "\uf06e"   // eye
#define ICON_FA_EDIT    "\uf044"   // edit
#define ICON_FA_TRASH   "\uf1f8"   // trash
#define ICON_FA_PLUS    "\uf067"   // plus
#define ICON_FA_MINUS   "\uf068"   // minus
#define ICON_FA_CHECK   "\uf00c"   // check
#define ICON_FA_TIMES   "\uf00d"   // times
#define ICON_FA_SEARCH  "\uf002"   // search
#define ICON_FA_DOWNLOAD "\uf019" // download
#define ICON_FA_UPLOAD  "\uf093"   // upload
#define ICON_FA_REFRESH "\uf021"   // refresh
#define ICON_FA_SETTINGS "\uf013" // settings
#define ICON_FA_INFO    "\uf05a"   // info
#define ICON_FA_WARNING "\uf071"   // warning
#define ICON_FA_ERROR   "\uf071"   // error/exclamation
#define ICON_FA_SUCCESS "\uf058"   // check-circle
#define ICON_FA_GEAR    "\uf013"   // gear
#define ICON_FA_LINK    "\uf0c1"   // link
#define ICON_FA_COPY    "\uf0c5"   // copy
#define ICON_FA_PASTE   "\uf0ea"   // paste
#define ICON_FA_PLAY    "\uf04b"   // play
#define ICON_FA_PAUSE   "\uf04c"   // pause
#define ICON_FA_STOP    "\uf04d"   // stop

namespace hunter {
namespace win32 {

// FontAwesome font loading
class FontAwesome {
public:
    static bool LoadFont();
    static bool IsLoaded();
    static const char* GetFontData();
    static size_t GetFontSize();
    
private:
    static bool font_loaded_;
    static std::string font_data_;
};

} // namespace win32
} // namespace hunter
