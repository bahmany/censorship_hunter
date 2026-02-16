# Iranian Free V2Ray ProGuard Rules

# Keep Application class
-keep class com.hunter.app.FreeV2RayApplication { *; }

# Keep services
-keep class com.hunter.app.VpnService { *; }
-keep class com.hunter.app.BootReceiver { *; }

# Keep activities
-keep class com.hunter.app.MainActivity { *; }
-keep class com.hunter.app.SettingsActivity { *; }
-keep class com.hunter.app.AppSelectActivity { *; }
-keep class com.hunter.app.ConfigListActivity { *; }

# Keep data classes
-keep class com.hunter.app.ConfigItem { *; }
-keep class com.hunter.app.ConfigAdapter { *; }
-keep class com.hunter.app.ConfigManager { *; }
-keep class com.hunter.app.ConfigManager$* { *; }
-keep class com.hunter.app.ConfigTester { *; }
-keep class com.hunter.app.ConfigTester$* { *; }
-keep class com.hunter.app.ConfigBalancer { *; }
-keep class com.hunter.app.ConfigMonitorService { *; }
-keep class com.hunter.app.ConfigShareActivity { *; }
-keep class com.hunter.app.XRayManager { *; }
-keep class com.hunter.app.XRayManager$* { *; }
-keep class com.hunter.app.V2RayConfigHelper { *; }

-keep class engine.** { *; }
-keep class go.** { *; }

# OkHttp
-dontwarn okhttp3.**
-dontwarn okio.**
-keep class okhttp3.** { *; }
-keep interface okhttp3.** { *; }

# Keep JSON parsing
-keep class org.json.** { *; }

# AndroidX
-keep class androidx.** { *; }
-dontwarn androidx.**

# Material Components
-keep class com.google.android.material.** { *; }
-dontwarn com.google.android.material.**
