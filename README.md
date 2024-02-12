# Dbus Binding

Sample binding for interfacing DBUS.

## Dependencies

This binding uses afb-binding, libjson-c and libsystemd.



## Compiling

Example:

```
mkdir build
cd build
cmake ..
make
```

It produces the binding `dbus-binding.so`.

## API

The binding v1 offers 5 verbs: `version`, `call`, `signal`, `subscribe`, `unsubscribe`.

### version

Takes no arguments.

Returns the STRINGZ representing is version.

### call

Make a call to DBUS method
The unique argument is a json object with:

- bus: optional string, : 'system' or 'user' (default is system)
- destination: string, the DBUS destination
- path: string, the DBUS path
- interface: string, the DBUS interface
- member: string, member of the interface
- signature: optional string, DBUS signature signature of the data
- data: mostly array, the data of the call

That call is synchronous and waits for the response.
The response is an JSON object

### signal

Send a DBUS signal
The unique argument is a json object with:

- bus: optional string, : 'system' or 'user' (default is system)
- destination: string, the DBUS destination
- path: string, the DBUS path
- interface: string, the DBUS interface
- member: string, member of the interface
- signature: optional string, DBUS signature signature of the data
- data: mostly array, the data of the call

### subscribe

Subscribe to a DBUS event.
The unique argument is a json object with:

- bus: optional string, : 'system' or 'user' (default is system)
- match: string, the DBUS match specification
- event: optional string, Name of the expected event (default is default)

### unsubscribe

Unsuscribe from a previous subscription.
Same content than subscribe.

## Examples

```
dbus version

dbus call {"bus":"user", "destination":"org.freedesktop.DBus", "path":"/org/freedesktop/DBus", "interface": "org.freedesktop.DBus", "member": "RequestName", "signature": "su", "data": [ "bzh.iot.dbus.binding", 0 ] }

dbus call {"bus":"user", "destination":"org.freedesktop.DBus", "path":"/org/freedesktop/DBus", "interface": "org.freedesktop.DBus", "member": "ListNames", "signature": "", "data": null }

dbus call {"bus":"system", "destination":"org.freedesktop.DBus", "path":"/org/freedesktop/DBus", "interface": "org.freedesktop.DBus", "member": "ListNames", "signature": "", "data": null }

dbus subscribe {"bus": "system", "match": "type=signal,sender=org.freedesktop.NetworkManager", "event":"nme"}

dbus subscribe {"bus": "system", "match": "type=signal,sender=org.freedesktop.NetworkManager,member=StateChanged", "event":"nme"}
dbus subscribe {"bus": "system", "match": "type=signal,sender=org.freedesktop.NetworkManager,member=StateChanged,interface=org.freedesktop.NetworkManager.Connection.Active", "event":"nme"}

```


