# huntercensor User Guide (English)

![huntercensor Main Interface](../Screenshot%202026-03-23%20135206.png)

## 1. What huntercensor is

`huntercensor` is a native Windows application for discovering, validating, copying, and provisioning censorship-resistant proxy configurations.

It is designed for two kinds of users:
- **Normal users** who want one simple dashboard and a few main actions.
- **Advanced users** who want deeper control over sources, runtime engines, logs, maintenance, and censorship analysis.

The app is intentionally designed to work **without administrator rights for normal daily use**.

## 2. Administrator rights policy

huntercensor does **not** require administrator rights for:
- launching the app
- installation through the normal installer
- scanning for configs
- background refresh
- validation
- copying configs
- showing QR codes
- importing/exporting configs
- reading logs
- network probing and discovery

On Windows, administrator approval may be requested **only** for the specific **Execute Bypass** operation inside the `Censorship` page, because route injection is a privileged system operation.

This means:
- the app starts normally as a regular user
- the installer can run in user mode
- most users should never need admin rights

## 3. Main pages

### Home
This is the main page for most users.

Use it for:
- starting or stopping the scan
- probing the network
- discovering an exit path
- copying all working configs
- copying one config
- saving one config to a file
- showing a QR code for mobile scan

### Configs
Use this page when you want to:
- browse more configs
- filter live vs visible configs
- copy one config or all visible configs
- import configs from a file
- export the current database

### Censorship
Use this page when you need:
- detailed network probe information
- exit-path discovery results
- traceroute hop inspection
- raw verification logs
- the optional edge-bypass workflow

### Logs
Use this page to:
- monitor live logs
- copy logs for support or debugging
- focus only on errors

### Advanced
Use this page only if you need:
- custom runtime paths
- source list management
- Telegram configuration
- maintenance actions
- port-level details

### About
Use this page to:
- see the project identity
- copy repository / releases / issues URLs
- find the user guides

## 4. Normal workflow for most users

### Step 1: Start the scan
Go to `Home` and click `Start Scan`.

What happens:
- the orchestrator starts collecting configs
- validation begins in the background
- working configs will appear gradually

### Step 2: Wait
This is important:
- sometimes working configs appear quickly
- sometimes it may take **hours**
- in difficult conditions it may take **days**

The app is designed for long-running discovery.

### Step 3: Copy or scan a working config
When working configs appear on `Home`:
- use `Copy All Working` to copy all unique working configs
- use `Copy` on a single row to copy one config
- use `Save` to write one config to a file
- use `QR` to show a QR code for your phone

## 5. Importing configs manually

Go to `Configs`.

You can:
- import from a file
- paste configs into the manual input area
- export the database to a text file

This is useful when you already have some configs and want huntercensor to validate or manage them.

## 6. Using QR codes

From `Home` or `Configs`, press `QR` next to a config.

In the QR window you can:
- view the QR code
- copy the URI
- save the URI to a file
- save the QR image as a bitmap

Typical phone workflow:
- open your proxy client on the phone
- choose import by QR code
- scan the QR from the computer screen

## 7. Censorship tools

The `Censorship` page is for analysis and troubleshooting.

### Probe Network
This checks network behavior and recommends a strategy.

### Discover Exit
This tries to discover routing context, gateway information, and a suggested exit path.

### Execute Bypass
This is an advanced action.
On Windows, this may ask for administrator approval because it attempts privileged route injection.

If Windows asks for permission:
- this request is for the bypass operation only
- scanning and ordinary use still remain non-admin

## 8. Installation notes

Preferred installation path:
- use the installer in user mode
- install under your local user profile

The app is designed for systems where the user may **not** have administrative install privileges.

`setup.bat` now works in user mode as well.
If it is not elevated, it simply skips firewall-rule setup instead of failing.

## 9. Troubleshooting

### No working configs yet
Possible reasons:
- the scan has just started
- the network is heavily restricted
- current public sources are weak or stale
- local runtime engines are missing or misconfigured

What to do:
- keep the app running longer
- check `Logs`
- verify runtime paths in `Advanced`
- import additional sources manually if needed

### Copy does not work
Possible reasons:
- there are no working configs yet
- clipboard access failed temporarily

What to do:
- check the toast message
- check the logs page
- retry the copy action

### QR code does not import on phone
Possible reasons:
- the target app does not support that URI type
- the screen is too small or blurred
- the URI itself is not accepted by the mobile client

What to do:
- try `Copy` instead
- save the URI to a text file
- try a different mobile client

### Execute Bypass asks for admin rights
This is expected on Windows.
It happens only for the specific bypass operation.

### Execute Bypass fails after elevation
Possible reasons:
- local route injection failed
- gateway/interface resolution was incomplete
- the network blocks verification probes

What to do:
- review the raw edge-bypass logs on the `Censorship` page
- use discovery again
- confirm the suggested path is realistic

## 10. Project links

Repository:
- `https://github.com/bahmany/censorship_hunter`

Issues:
- `https://github.com/bahmany/censorship_hunter/issues`

Releases:
- `https://github.com/bahmany/censorship_hunter/releases`

## 11. Recommended usage model

For normal users:
- open `Home`
- click `Start Scan`
- wait
- copy or scan a working config

For advanced users:
- use `Configs`, `Censorship`, `Logs`, and `Advanced`
- adjust sources and runtime paths
- inspect logs and probe output

## 12. Final reminder

huntercensor is built for real long-running discovery, not fake demo success.
If the network is difficult, the app may need significant time before it finds good results.
