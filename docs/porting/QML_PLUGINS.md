# Trusted QML plugins

APM Planner 3.0 uses a hybrid user interface. The application shell, docking,
configuration tables and core services remain Qt Widgets/C++. Dynamic panels,
instruments and user extensions may use QML. Plugins are trusted code loaded in
the application process: there is no sandbox, permission model or capability
declaration.

All user plugins use one application-owned `QQmlEngine`. The engine exports the
module `APMPlanner.Core 1.0` and its singleton `APMPlanner`. The singleton gives
direct access to the current `uasManager`, `activeVehicle`, `linkManager`,
`vehicleTargetManager`, `vehicleCommandService`, `parameterService`, settings,
`missionCommandCatalog`, main window, plugin manager, application metadata and
resource/data paths.
Public API within one major is extended without renaming existing
members. A new API major may break compatibility; plugin authors are responsible
for updating their plugin.

## Location and manifest

The manager scans immediate child directories of:

- `<application resources>/qml-plugins`;
- `<writable application data>/qml-plugins`.

Each child contains `apm-plugin.json`:

```json
{
  "manifestVersion": 1,
  "id": "org.example.battery-monitor",
  "name": "Battery Monitor",
  "version": "1.0.0",
  "apiMajor": 1,
  "main": "Main.qml",
  "ui": {
    "type": "toolPage",
    "title": "Battery Monitor",
    "icon": "battery.svg"
  }
}
```

`description`, `author` and `ui.icon` are optional. Version 1 deliberately has
only the `toolPage` contribution. Valid plugins appear under Tools → QML Plugins
and open lazily in a modeless `QQuickWidget` window. Relative QML components and
assets remain local to the plugin directory.

## QML entry point

```qml
import QtQuick 2.12
import QtQuick.Controls 2.5
import APMPlanner.Core 1.0

Item {
    width: 640
    height: 480

    Text {
        text: plugin.name + " / API " + APMPlanner.apiMajor
    }

    Component.onCompleted: {
        console.log(APMPlanner.uasManager)
        console.log(APMPlanner.linkManager)
        plugin.setSetting("lastOpened", new Date().toISOString())
    }
}
```

The per-plugin `plugin` object exposes `id`, `name`, `version`, `path`,
`setting()`, `setSetting()` and `removeSetting()`. `APMPlanner.settings` is also
available for direct access to the application settings because plugins are
trusted.

### Planner unit presentation

`APMPlanner.mainWindow` exposes live, typed unit properties for PLAN. Their
accepted values deliberately match Mission Planner's persisted values exactly:

```qml
readonly property string altitudeUnits:
    APMPlanner.mainWindow ? APMPlanner.mainWindow.plannerAltitudeUnits : "Meters"
readonly property string distanceUnits:
    APMPlanner.mainWindow ? APMPlanner.mainWindow.plannerDistanceUnits : "Meters"

function showImperialPlan() {
    APMPlanner.mainWindow.plannerAltitudeUnits = "Feet"
    APMPlanner.mainWindow.plannerDistanceUnits = "Feet"
}
```

Both properties accept only `Meters` or `Feet` (case-insensitively), notify QML
bindings on live changes and persist the canonical spelling. They affect display
wrappers only: plugin code that reads or writes mission, terrain, radius or
MAVLink values must continue to use canonical SI units. `plannerDistanceUnits`
formats short PLAN distances in `m|ft` and route totals in `km|miles`; Survey
geometry and polygon offsets remain explicitly SI.

### Mission command catalog

`APMPlanner.missionCommandCatalog` and
`APMPlanner.service("missionCommandCatalog")` return the same C++-owned catalog
used by the PLAN command editor and the SETUP → Advanced → Mission Command List
page. It merges the MAVLink commands compiled into APM Planner with user-defined
IDs and seven optional parameter-label overrides.

```qml
var catalog = APMPlanner.missionCommandCatalog
console.log(catalog.commandNames)
console.log(catalog.GetId("WAYPOINT"))
console.log(catalog.EffectiveLabels(16))

catalog.SaveDefinitionMaps([
    {
        "id": 61000,
        "name": "VENDOR_SCAN",
        "parameterLabels": ["Rows", "", "", "", "Lat", "Lon", "Alt"]
    }
])
```

`definitions` contains maps with `id`, `name` and a normalized seven-string
`parameterLabels` list. `GetName(id)`, `GetId(name)`, `GetLabels(id)` and
`EffectiveLabels(id)` provide direct lookup. `SaveDefinitionMaps()` validates
the complete candidate before changing settings; IDs and names must be unique,
known IDs retain their canonical MAVLink name, and a custom ID cannot reuse a
known name. Successful Save or `Reload()` increments `revision` and emits
`catalogChanged`, immediately refreshing PLAN without creating mission undo
state.

For exact Mission Planner 10 interoperability the persisted settings are
`PlannerExtraCommand` (name to seven labels) and `PlannerExtraCommandIDs`
(custom name to unsigned 16-bit ID). A plugin that writes these raw settings
directly must call `catalog.Reload()` afterwards so native and QML views receive
the refresh signal.

## Exact MAVLink target registry

`APMPlanner.vehicleTargetManager` is the application-owned registry of visible
MAVLink endpoints. `APMPlanner.service("vehicleTargetManager")` returns the same
object. Endpoint identity is always the exact `(linkId, systemId, componentId)`
tuple: equal system/component ids received through two physical links remain two
different targets.

The object exposes these live QML properties:

| Property | Meaning |
|---|---|
| `endpoints` | Deterministically ordered list of visible endpoint maps |
| `currentTarget` | Selected endpoint map, or `{ valid: false }`; always includes `generation` |
| `hasCurrentTarget` | Whether the selected tuple still exists |
| `revision` | Registry-topology and metadata revision |
| `targetGeneration` | Selection/lifetime generation used to reject stale work |

Every endpoint map contains `valid`, `linkId`, `systemId`, `componentId`,
`linkName`, `componentName` and `displayName`. As in Mission Planner, only
sources of `HEARTBEAT`, `HIGH_LATENCY2` and `UAVCAN_NODE_STATUS` become visible.
Endpoints do not disappear merely because heartbeat data becomes quiet; removing
their link removes them, and removal of the selected link invalidates the target
atomically without silently selecting a different vehicle.

```qml
function chooseTarget(endpoint) {
    var targets = APMPlanner.vehicleTargetManager
    var generation = targets.targetGeneration
    return targets.selectTargetIfGeneration(endpoint.linkId,
                                            endpoint.systemId,
                                            endpoint.componentId,
                                            generation)
}

function stillOwnsTarget(snapshot) {
    return APMPlanner.vehicleTargetManager.isCurrentTarget(
                snapshot.linkId, snapshot.systemId, snapshot.componentId,
                snapshot.generation)
}
```

`selectTarget(linkId, systemId, componentId)` selects an existing exact tuple;
`selectTargetIfGeneration(...)` additionally rejects a stale selection race;
`clearTarget()` explicitly invalidates it. Native DATA vehicle actions now
capture an immutable target lease and send `COMMAND_LONG` through the exact
selected link/system/component; stale or unavailable exact targets fail closed,
and `COMMAND_ACK` is correlated to the same endpoint and command.

`APMPlanner.vehicleCommandService` (also available through
`service("vehicleCommandService")`) exposes that transport to trusted QML:

```qml
var commands = APMPlanner.vehicleCommandService
var result = commands.sendCurrentCommandLong(
        20, 0,                       // MAV_CMD_NAV_RETURN_TO_LAUNCH
        0, 0, 0, 0, 0, 0, 0)
```

The result is `0 Sent`, `1 InvalidTarget`, `2 StaleTarget` or
`3 TransportUnavailable`. `commandAckReceived` includes generation and the full
link/system/component identity. Raw `activeVehicle` command slots and mission
sends still use legacy routing, so new plugins should use this exact service for
`COMMAND_LONG`.

## Exact parameter service

`APMPlanner.parameterService` and
`APMPlanner.service("parameterService")` return the same application-owned
object. Unlike the raw `activeVehicle` parameter methods, it always binds an
operation to the current target generation and writes to one exact physical
link. A target switch or link removal cancels pending work; a late response from
the old endpoint is ignored and cannot mutate its committed cache.

The initial QML surface is:

```qml
var parameters = APMPlanner.parameterService
var result = parameters.requestCurrentParameterList()
result = parameters.requestCurrentParameterRead("ARMING_CHECK")

// Preferred API: the wire type comes from the committed exact snapshot.
var transactionId = parameters.writeCurrentParameter("RTL_ALT", 1500)
var batchId = parameters.writeCurrentParameters([
    { "name": "ATC_RAT_RLL_P", "value": 0.14 },
    { "name": "ATC_RAT_PIT_P", "value": 0.14 },
    { "name": "FEATURE_ENABLE", "value": 1 }
])
parameters.cancelParameterWrite(transactionId)

var value = parameters.currentParameterValue("RTL_ALT")
var snapshot = parameters.currentParameters() // name/value/type/index/count maps
```

`writeCurrentParameter()` returns a non-zero transaction id when accepted, and
`writeCurrentParameters()` returns a non-zero batch id after the whole batch was
validated. Zero means rejection without traffic. The service derives each wire
type from the committed snapshot, normalizes values through the selected MAVLink
encoding, skips a no-op unless `force` is true, and writes only one parameter at
a time. A batch is sorted alphabetically with case-sensitive `*ENABLE`
parameters deliberately written last.

The lower-level `setCurrentParameter(name, value, type)` remains available for
core diagnostics. Its integer result is `0 Sent`, `1 InvalidTarget`, `2 StaleTarget`,
`3 InvalidParameter` or `4 TransportUnavailable`; `Sent` means accepted by the
serialized queue, while a later initial-send failure is reported by
`parameterWriteFailed`. Classic MAVLink parameter type
numbers are preserved (`1..10`, for `UINT8` through `REAL64`); values not
representable by classic `PARAM_SET` fail with `InvalidParameter`. ArduPilot
float-cast versus bytewise-union encoding is selected from HEARTBEAT and
AUTOPILOT_VERSION capabilities.

The writer sends once, waits 700 ms for matching `PARAM_VALUE`, and retries at
most three times. It emits `parameterWriteStarted`, `parameterWriteRetried`,
`parameterWriteCompleted`, `parameterWriteSkipped`, `parameterWriteFailed`,
`parameterWriteCancelled`, `parameterBatchProgress` and
`parameterBatchCompleted`; the legacy `parameterWriteAcknowledged` success signal
is retained. Because classic `PARAM_VALUE` has no request id, a same-name echo
with a different type or value becomes authoritative in the cache but is not
treated as either success or terminal rejection; the write remains pending for
an exact echo or timeout. No-op state, committed name/type and negotiated
encoding are revalidated when a queued item actually becomes active. Full-list
refresh and writes never overlap: a refresh forms a FIFO boundary and later
writes wait for its atomic commit, failure or cancellation. High-level methods
return their transaction/batch id before a no-op, synchronous echo or
initial-send failure can publish a terminal signal.

Completion signals
include the target generation and complete link/system/component envelope, so a
plugin can validate asynchronous UI state. `currentParameters()` exposes only
the committed snapshot: an incomplete, failed or cancelled refresh never
replaces the last complete list. Parameter names are Latin-1 MAVLink ids of at
most 16 bytes.

The old synchronous getters on `activeVehicle` remain available while native
CONFIG/SETUP widgets migrate, but their transport slots are legacy and must not
be used for exact writes from new plugins.

## Active vehicle read API

`APMPlanner.activeVehicle` is the raw application-owned vehicle `QObject`, or
`null` when there is no active vehicle. `APMPlanner.service("activeVehicle")`
returns the same object. The following synchronous snapshot getters on the
singleton forward to the current vehicle while preserving the names of the C++
`UASInterface` getters:

```cpp
Q_INVOKABLE int getUASID() const;
Q_INVOKABLE QString getUASName() const;
Q_INVOKABLE bool isArmed() const;
Q_INVOKABLE QString getShortMode() const;
```

| QML call | Underlying `UASInterface` getter | Result when `activeVehicle` is `null` |
|---|---|---|
| `APMPlanner.getUASID()` | `int getUASID() const` | `-1` |
| `APMPlanner.getUASName()` | `QString getUASName() const` | empty string |
| `APMPlanner.isArmed()` | `bool isArmed() const` | `false` |
| `APMPlanner.getShortMode()` | `const QString &getShortMode() const` | empty string |

The concrete `UAS` object already exports these cached telemetry properties, so
the singleton does not duplicate them:

| Raw `activeVehicle` property | Type and unit |
|---|---|
| `latitude`, `longitude` | `double`, degrees |
| `altitudeRelative` | `double`, metres above the vehicle home reference |
| `roll`, `pitch`, `yaw` | `double`, radians |

### Cached parameters

The raw `activeVehicle` also exposes a read-only snapshot of the parameter
values already received by the core:

```cpp
Q_INVOKABLE QList<int> getComponentIds();
Q_INVOKABLE QList<QString> getParameterNames(int component);
Q_INVOKABLE QVariant getParameterValue(
    int component, const QString &parameter) const;
```

In QML the two list results are JavaScript-compatible sequences:

```qml
function cachedParameter(component, name) {
    var vehicle = APMPlanner.activeVehicle
    if (!vehicle)
        return undefined

    var components = vehicle.getComponentIds()
    var names = vehicle.getParameterNames(component)
    var value = vehicle.getParameterValue(component, name)
    return value
}
```

These calls only inspect the vehicle's local parameter cache. They never send a
MAVLink message, wait for a response or start a parameter download. Component
and name lists may therefore be empty or incomplete until values have been
received. An unknown component produces an empty name list; an unknown
component/name pair produces an invalid `QVariant`, seen by QML as a nullish
(`undefined` or `null`) value. List ordering is not an API contract.

`requestParameters()` and `requestParameter(component, name)` are separate raw
vehicle command slots. They perform network I/O and are not implied by any of
the snapshot getters. Plugins that choose to invoke them must handle asynchronous
updates themselves.

## DATA vehicle actions

A trusted plugin can contribute an action to the DATA page `ActionSelector`.
This is a direct in-process callback contract, not a command facade:

```cpp
Q_PROPERTY(QStringList vehicleActions READ vehicleActions
           NOTIFY vehicleActionsChanged)
Q_INVOKABLE bool registerVehicleAction(
    const QString &name, const QJSValue &callback);
Q_INVOKABLE bool registerVehicleAction(
    const QString &name, const QJSValue &callback,
    const QVariantMap &options);
Q_INVOKABLE bool unregisterVehicleAction(const QString &name);
```

The name is trimmed and must be non-empty and unique among plugin actions. The
callback must be callable. The optional map accepts only `after` and `before`,
whose string values name an existing selector item. An unknown anchor appends
the action; when both anchors resolve, `before` determines the final position.
Names that duplicate a built-in selector caption are reserved and are not
shown. Invalid or duplicate registration returns `false`; unregistering an
unknown name also returns `false`.

```qml
Item {
    readonly property string actionName: "Capture Survey Marker"

    Component.onCompleted: {
        if (!APMPlanner.registerVehicleAction(
                actionName,
                function(selectedName) {
                    var vehicle = APMPlanner.activeVehicle
                    if (!vehicle)
                        throw new Error("No active vehicle")

                    // Trusted code may call the documented/raw vehicle API
                    // directly. The plugin owns the operation and its result.
                    console.log(selectedName, APMPlanner.getUASName())
                },
                { "after": "Trigger Camera" })) {
            console.log("Could not register " + actionName)
        }
    }

    Component.onDestruction:
        APMPlanner.unregisterVehicleAction(actionName)
}
```

Selection invokes the callback synchronously on the GUI thread and passes its
registered name as the only argument. A JavaScript exception is reported in the
DATA action-status field. The host does not check the active vehicle, send a
MAVLink command, wait for `COMMAND_ACK`, retry, or roll back plugin work. The
callback can use the raw `activeVehicle` and other core objects with full process
authority and is responsible for null checks, confirmation, command correlation
and error handling appropriate to its operation.

Registrations last until explicit unregister or plugin-manager shutdown. Tool
pages are instantiated lazily, so a registration in `Component.onCompleted`
appears only after that plugin page has first been opened. The manager clears
all registrations when plugins are rescanned or shut down; plugin code must not
retain the borrowed core objects beyond their documented lifetime.

`Format_SD_Card` is a built-in DATA action rather than a QML compatibility
shim. It warns that the first vehicle storage device will be erased, then sends
`MAV_CMD_STORAGE_FORMAT` with confirmation `1`, parameter 1 = `1` and parameter
2 = `1`. With an applicable exact selection it targets that selected component
and accepts only its exact-link `COMMAND_ACK`; the pre-selector compatibility
path retains the legacy primary-component behavior.

For example, a plugin can bind telemetry directly and refresh identity/state
snapshots when the active object changes:

```qml
Item {
    id: root

    property int vehicleId: -1
    property string vehicleName: ""
    property bool armed: false
    property string mode: ""
    property real latitude: APMPlanner.activeVehicle
                            ? APMPlanner.activeVehicle.latitude : 0

    function refreshVehicleState() {
        vehicleId = APMPlanner.getUASID()
        vehicleName = APMPlanner.getUASName()
        armed = APMPlanner.isArmed()
        mode = APMPlanner.getShortMode()
    }

    Component.onCompleted: refreshVehicleState()
    Connections {
        target: APMPlanner
        onActiveVehicleChanged: root.refreshVehicleState()
    }
}
```

The getter results are the latest values cached by the core; calling them does
not request telemetry from the vehicle. They are ordinary invokable methods,
not notifying properties. A plugin that keeps identity, arming or mode state
must refresh it after `activeVehicleChanged` and after the corresponding raw
vehicle signals (`nameChanged`, `armingChanged`, `modeChanged`).

The `activeVehicle` pointer is borrowed. Plugins must not delete or reparent it,
and should reacquire it after every `activeVehicleChanged`. The signal is emitted
when the selection changes and when the selected object is destroyed; in the
latter case `activeVehicle` and `service("activeVehicle")` are already `null`.

The shared QML engine, plugin windows and exported core objects live on the GUI
thread. Direct property reads and method calls are synchronous and must be made
on that thread. Plugins must not block a getter or signal handler, and must use
queued hand-off when their own worker thread needs core state. The raw object may
expose additional implementation properties or slots, but only the members
documented here are part of this read API major.

Plugin QML executes with the same process permissions as APM Planner. A plugin
can block the GUI thread, consume memory, access Qt file/network APIs and invoke
flight operations exposed by the core. Users should install only code they
trust.
